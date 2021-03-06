/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "psimediaprovider.h"

#include <QStringList>
#include <QImage>
#include <QMutex>
#include <QWaitCondition>
#include <QTime>
#include <QtPlugin>
#include <QIODevice>
#include "devices.h"
#include "modes.h"
#include "gstthread.h"
#include "rwcontrol.h"

#ifdef QT_GUI_LIB
#include <QWidget>
#include <QPainter>
#endif

namespace PsiMedia {

static PDevice gstDeviceToPDevice(const GstDevice &dev, PDevice::Type type)
{
	PDevice out;
	out.type = type;
	out.name = dev.name;
	out.id = dev.id;
	return out;
}

//----------------------------------------------------------------------------
// GstVideoWidget
//----------------------------------------------------------------------------
class GstVideoWidget : public QObject
{
	Q_OBJECT

public:
	VideoWidgetContext *context;
	QImage curImage;

	GstVideoWidget(VideoWidgetContext *_context, QObject *parent = 0) :
		QObject(parent),
		context(_context)
	{
		QPalette palette;
		palette.setColor(context->qwidget()->backgroundRole(), Qt::black);
		context->qwidget()->setPalette(palette);
		context->qwidget()->setAutoFillBackground(true);

		connect(context->qobject(), SIGNAL(resized(const QSize &)), SLOT(context_resized(const QSize &)));
		connect(context->qobject(), SIGNAL(paintEvent(QPainter *)), SLOT(context_paintEvent(QPainter *)));
	}

	void show_frame(const QImage &image)
	{
		curImage = image;
		context->qwidget()->update();
	}

private slots:
	void context_resized(const QSize &newSize)
	{
		Q_UNUSED(newSize);
	}

	void context_paintEvent(QPainter *p)
	{
		if(curImage.isNull())
			return;

		QSize size = context->qwidget()->size();
		QSize newSize = curImage.size();
		newSize.scale(size, Qt::KeepAspectRatio);
		int xoff = 0;
		int yoff = 0;
		if(newSize.width() < size.width())
			xoff = (size.width() - newSize.width()) / 2;
		else if(newSize.height() < size.height())
			yoff = (size.height() - newSize.height()) / 2;

		// ideally, the backend will follow desired_size() and give
		//   us images that generally don't need resizing
		QImage i;
		if(curImage.size() != newSize)
		{
			// the IgnoreAspectRatio is okay here, since we
			//   used KeepAspectRatio earlier
			i = curImage.scaled(newSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		}
		else
			i = curImage;

		p->drawImage(xoff, yoff, i);
	}
};

//----------------------------------------------------------------------------
// GstFeaturesContext
//----------------------------------------------------------------------------
static QList<PDevice> get_audioOutputDevices()
{
	QList<PDevice> list;
	foreach(const GstDevice &i, devices_list(PDevice::AudioOut))
		list += gstDeviceToPDevice(i, PDevice::AudioOut);
	return list;
}

static QList<PDevice> get_audioInputDevices()
{
	QList<PDevice> list;
	foreach(const GstDevice &i, devices_list(PDevice::AudioIn))
		list += gstDeviceToPDevice(i, PDevice::AudioIn);
	return list;
}

static QList<PDevice> get_videoInputDevices()
{
	QList<PDevice> list;
	foreach(const GstDevice &i, devices_list(PDevice::VideoIn))
		list += gstDeviceToPDevice(i, PDevice::VideoIn);
	return list;
}

class FeaturesThread : public QThread
{
        Q_OBJECT

public:
	int types;
	PFeatures results;

	FeaturesThread(QObject *parent = 0) :
		QThread(parent)
	{
	}

	virtual void run()
	{
		PFeatures out;
		if(types & FeaturesContext::AudioOut)
			out.audioOutputDevices = get_audioOutputDevices();
		if(types & FeaturesContext::AudioIn)
			out.audioInputDevices = get_audioInputDevices();
		if(types & FeaturesContext::VideoIn)
			out.videoInputDevices = get_videoInputDevices();
		if(types & FeaturesContext::AudioModes)
			out.supportedAudioModes = modes_supportedAudio();
		if(types & FeaturesContext::VideoModes)
			out.supportedVideoModes = modes_supportedVideo();
		results = out;
	}
};

class GstFeaturesContext : public QObject, public FeaturesContext
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::FeaturesContext)

public:
	GstThread *gstThread;
	FeaturesThread *thread;

	GstFeaturesContext(GstThread *_gstThread, QObject *parent = 0) :
		QObject(parent),
		gstThread(_gstThread)
	{
		thread = new FeaturesThread(this);
		connect(thread, SIGNAL(finished()), SIGNAL(finished()));
	}

	~GstFeaturesContext()
	{
		thread->wait();
		delete thread;
	}

	virtual QObject *qobject()
	{
		return this;
	}

	virtual void lookup(int types)
	{
		if(types > 0)
		{
			thread->types = types;
			thread->start();
		}
	}

	virtual bool waitForFinished(int msecs)
	{
		return thread->wait(msecs < 0 ? ULONG_MAX : msecs);
	}

	virtual PFeatures results() const
	{
		return thread->results;
	}

signals:
	void finished();
};

//----------------------------------------------------------------------------
// GstRtpChannel
//----------------------------------------------------------------------------
// for a live transmission we really shouldn't have excessive queuing (or
//   *any* queuing!), so we'll cap the queue sizes.  if the system gets
//   overloaded and the thread scheduling skews such that our queues get
//   filled before they can be emptied, then we'll start dropping old
//   items making room for new ones.  on a live transmission there's no
//   sense in keeping ancient data around.  we just drop and move on.
#define QUEUE_PACKET_MAX 25

// don't wake the main thread more often than this, for performance reasons
#define WAKE_PACKET_MIN 40

class GstRtpSessionContext;

class GstRtpChannel : public QObject, public RtpChannelContext
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::RtpChannelContext)

public:
	bool enabled;
	QMutex m;
	GstRtpSessionContext *session;
	QList<PRtpPacket> in;

	//QTime wake_time;
	bool wake_pending;
	QList<PRtpPacket> pending_in;

	int written_pending;

	GstRtpChannel() :
		QObject(),
		enabled(false),
		wake_pending(false),
		written_pending(0)
	{
	}

	virtual QObject *qobject()
	{
		return this;
	}

	virtual void setEnabled(bool b)
	{
		QMutexLocker locker(&m);
		enabled = b;
	}

	virtual int packetsAvailable() const
	{
		return in.count();
	}

	virtual PRtpPacket read()
	{
		return in.takeFirst();
	}

	virtual void write(const PRtpPacket &rtp)
	{
		m.lock();
		if(!enabled)
			return;
		m.unlock();

		receiver_push_packet_for_write(rtp);
		++written_pending;

		// only queue one call per eventloop pass
		if(written_pending == 1)
			QMetaObject::invokeMethod(this, "processOut", Qt::QueuedConnection);
	}

	// session calls this, which may be in another thread
	void push_packet_for_read(const PRtpPacket &rtp)
	{
		QMutexLocker locker(&m);
		if(!enabled)
			return;

		// if the queue is full, bump off the oldest to make room
		if(pending_in.count() >= QUEUE_PACKET_MAX)
			pending_in.removeFirst();

		pending_in += rtp;

		// TODO: use WAKE_PACKET_MIN and wake_time ?

		if(!wake_pending)
		{
			wake_pending = true;
			QMetaObject::invokeMethod(this, "processIn", Qt::QueuedConnection);
		}
	}

signals:
	void readyRead();
	void packetsWritten(int count);

private slots:
	void processIn()
	{
		int oldcount = in.count();

		m.lock();
		wake_pending = false;
		in += pending_in;
		pending_in.clear();
		m.unlock();

		if(in.count() > oldcount)
			emit readyRead();
	}

	void processOut()
	{
		int count = written_pending;
		written_pending = 0;
		emit packetsWritten(count);
	}

private:
	void receiver_push_packet_for_write(const PRtpPacket &rtp);
};

//----------------------------------------------------------------------------
// GstRecorder
//----------------------------------------------------------------------------
class GstRecorder : public QObject
{
	Q_OBJECT

public:
	RwControlLocal *control;
	QIODevice *recordDevice, *nextRecordDevice;
	bool record_cancel;

	QMutex m;
	bool wake_pending;
	QList<QByteArray> pending_in;

	GstRecorder(QObject *parent = 0) :
		QObject(parent),
		control(0),
		recordDevice(0),
		nextRecordDevice(0),
		record_cancel(false),
		wake_pending(false)
	{
	}

	void setDevice(QIODevice *dev)
	{
		Q_ASSERT(!recordDevice);
		Q_ASSERT(!nextRecordDevice);

		if(control)
		{
			recordDevice = dev;

			RwControlRecord record;
			record.enabled = true;
			control->setRecord(record);
		}
		else
		{
			// queue up the device for later
			nextRecordDevice = dev;
		}
	}

	void stop()
	{
		Q_ASSERT(recordDevice || nextRecordDevice);
		Q_ASSERT(!record_cancel);

		if(nextRecordDevice)
		{
			// if there was only a queued device, then there's
			//   nothing to do but dequeue it
			nextRecordDevice = 0;
		}
		else
		{
			record_cancel = true;

			RwControlRecord record;
			record.enabled = false;
			control->setRecord(record);
		}
	}

	void startNext()
	{
		if(control && !recordDevice && nextRecordDevice)
		{
			recordDevice = nextRecordDevice;
			nextRecordDevice = 0;

			RwControlRecord record;
			record.enabled = true;
			control->setRecord(record);
		}
	}

	// session calls this, which may be in another thread
	void push_data_for_read(const QByteArray &buf)
	{
		QMutexLocker locker(&m);
		pending_in += buf;
		if(!wake_pending)
		{
			wake_pending = true;
			QMetaObject::invokeMethod(this, "processIn", Qt::QueuedConnection);
		}
	}

signals:
	void stopped();

private slots:
	void processIn()
	{
		m.lock();
		wake_pending = false;
		QList<QByteArray> in = pending_in;
		pending_in.clear();
		m.unlock();

		QPointer<QObject> self = this;

		while(!in.isEmpty())
		{
			QByteArray buf = in.takeFirst();

			if(!buf.isEmpty())
			{
				recordDevice->write(buf);
			}
			else // EOF
			{
				recordDevice->close();
				recordDevice = 0;

				bool wasCancelled = record_cancel;
				record_cancel = false;

				if(wasCancelled)
				{
					emit stopped();
					if(!self)
						return;
				}
			}
		}
	}
};

//----------------------------------------------------------------------------
// GstRtpSessionContext
//----------------------------------------------------------------------------
class GstRtpSessionContext : public QObject, public RtpSessionContext
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::RtpSessionContext)

public:
	GstThread *gstThread;

	RwControlLocal *control;
	RwControlConfigDevices devices;
	RwControlConfigCodecs codecs;
	RwControlTransmit transmit;
	RwControlStatus lastStatus;
	bool isStarted;
	bool isStopping;
	bool pending_status;

#ifdef QT_GUI_LIB
	GstVideoWidget *outputWidget, *previewWidget;
#endif

	GstRecorder recorder;

	// keep these parentless, so they can switch threads
	GstRtpChannel audioRtp;
	GstRtpChannel videoRtp;
	QMutex write_mutex;
	bool allow_writes;

	GstRtpSessionContext(GstThread *_gstThread, QObject *parent = 0) :
		QObject(parent),
		gstThread(_gstThread),
		control(0),
		isStarted(false),
		isStopping(false),
		pending_status(false),
		recorder(this),
		allow_writes(false)
	{
#ifdef QT_GUI_LIB
		outputWidget = 0;
		previewWidget = 0;
#endif

		devices.audioOutVolume = 100;
		devices.audioInVolume = 100;

		codecs.useLocalAudioParams = true;
		codecs.useLocalVideoParams = true;

		audioRtp.session = this;
		videoRtp.session = this;

		connect(&recorder, SIGNAL(stopped()), SLOT(recorder_stopped()));
	}

	~GstRtpSessionContext()
	{
		cleanup();
	}

	virtual QObject *qobject()
	{
		return this;
	}

	void cleanup()
	{
		if(outputWidget)
			outputWidget->show_frame(QImage());
		if(previewWidget)
			previewWidget->show_frame(QImage());

		codecs = RwControlConfigCodecs();

		isStarted = false;
		isStopping = false;
		pending_status = false;

		recorder.control = 0;

		write_mutex.lock();
		allow_writes = false;
		delete control;
		control = 0;
		write_mutex.unlock();
	}

	virtual void setAudioOutputDevice(const QString &deviceId)
	{
		devices.audioOutId = deviceId;
		if(control)
			control->updateDevices(devices);
	}

	virtual void setAudioInputDevice(const QString &deviceId)
	{
		devices.audioInId = deviceId;
		devices.fileNameIn.clear();
		devices.fileDataIn.clear();
		if(control)
			control->updateDevices(devices);
	}

	virtual void setVideoInputDevice(const QString &deviceId)
	{
		devices.videoInId = deviceId;
		devices.fileNameIn.clear();
		devices.fileDataIn.clear();
		if(control)
			control->updateDevices(devices);
	}

	virtual void setFileInput(const QString &fileName)
	{
		devices.fileNameIn = fileName;
		devices.audioInId.clear();
		devices.videoInId.clear();
		devices.fileDataIn.clear();
		if(control)
			control->updateDevices(devices);
	}

	virtual void setFileDataInput(const QByteArray &fileData)
	{
		devices.fileDataIn = fileData;
		devices.audioInId.clear();
		devices.videoInId.clear();
		devices.fileNameIn.clear();
		if(control)
			control->updateDevices(devices);
	}

	virtual void setFileLoopEnabled(bool enabled)
	{
		devices.loopFile = enabled;
		if(control)
			control->updateDevices(devices);
	}

#ifdef QT_GUI_LIB
        virtual void setVideoOutputWidget(VideoWidgetContext *widget)
	{
		// no change?
		if(!outputWidget && !widget)
			return;
		if(outputWidget && outputWidget->context == widget)
			return;

		delete outputWidget;
		outputWidget = 0;

		if(widget)
			outputWidget = new GstVideoWidget(widget, this);

		devices.useVideoOut = widget ? true : false;
		if(control)
			control->updateDevices(devices);
	}

	virtual void setVideoPreviewWidget(VideoWidgetContext *widget)
	{
		// no change?
		if(!previewWidget && !widget)
			return;
		if(previewWidget && previewWidget->context == widget)
			return;

		delete previewWidget;
		previewWidget = 0;

		if(widget)
			previewWidget = new GstVideoWidget(widget, this);

		devices.useVideoPreview = widget ? true : false;
		if(control)
			control->updateDevices(devices);
	}
#endif

	virtual void setRecorder(QIODevice *recordDevice)
	{
		// can't assign a new recording device after stopping
		Q_ASSERT(!isStopping);

		recorder.setDevice(recordDevice);
	}

	virtual void stopRecording()
	{
		recorder.stop();
	}

	virtual void setLocalAudioPreferences(const QList<PAudioParams> &params)
	{
		codecs.useLocalAudioParams = true;
		codecs.localAudioParams = params;
	}

	virtual void setLocalVideoPreferences(const QList<PVideoParams> &params)
	{
		codecs.useLocalVideoParams = true;
		codecs.localVideoParams = params;
	}

	virtual void setMaximumSendingBitrate(int kbps)
	{
		codecs.maximumSendingBitrate = kbps;
	}

	virtual void setRemoteAudioPreferences(const QList<PPayloadInfo> &info)
	{
		codecs.useRemoteAudioPayloadInfo = true;
		codecs.remoteAudioPayloadInfo = info;
	}

	virtual void setRemoteVideoPreferences(const QList<PPayloadInfo> &info)
	{
		codecs.useRemoteVideoPayloadInfo = true;
		codecs.remoteVideoPayloadInfo = info;
	}

	virtual void start()
	{
		Q_ASSERT(!control && !isStarted);

		write_mutex.lock();

		control = new RwControlLocal(gstThread, this);
		connect(control, SIGNAL(statusReady(const RwControlStatus &)), SLOT(control_statusReady(const RwControlStatus &)));
		connect(control, SIGNAL(previewFrame(const QImage &)), SLOT(control_previewFrame(const QImage &)));
		connect(control, SIGNAL(outputFrame(const QImage &)), SLOT(control_outputFrame(const QImage &)));
		connect(control, SIGNAL(audioOutputIntensityChanged(int)), SLOT(control_audioOutputIntensityChanged(int)));
		connect(control, SIGNAL(audioInputIntensityChanged(int)), SLOT(control_audioInputIntensityChanged(int)));

		control->app = this;
		control->cb_rtpAudioOut = cb_control_rtpAudioOut;
		control->cb_rtpVideoOut = cb_control_rtpVideoOut;
		control->cb_recordData = cb_control_recordData;

		allow_writes = true;
		write_mutex.unlock();

		recorder.control = control;

		lastStatus = RwControlStatus();
		isStarted = false;
		pending_status = true;
		control->start(devices, codecs);
	}

	virtual void updatePreferences()
	{
		Q_ASSERT(control && !pending_status);

		pending_status = true;
		control->updateCodecs(codecs);
	}

	virtual void transmitAudio()
	{
		transmit.useAudio = true;
		control->setTransmit(transmit);
	}

	virtual void transmitVideo()
	{
		transmit.useVideo = true;
		control->setTransmit(transmit);
	}

	virtual void pauseAudio()
	{
		transmit.useAudio = false;
		control->setTransmit(transmit);
	}

	virtual void pauseVideo()
	{
		transmit.useVideo = false;
		control->setTransmit(transmit);
	}

	virtual void stop()
	{
		Q_ASSERT(control && !isStopping);

		// note: it's possible to stop even if pending_status is
		//   already true.  this is so we can stop a session that
		//   is in the middle of starting.

		isStopping = true;
		pending_status = true;
		control->stop();
	}

	virtual QList<PPayloadInfo> localAudioPayloadInfo() const
	{
		return lastStatus.localAudioPayloadInfo;
	}

	virtual QList<PPayloadInfo> localVideoPayloadInfo() const
	{
		return lastStatus.localVideoPayloadInfo;
	}

	virtual QList<PPayloadInfo> remoteAudioPayloadInfo() const
	{
		return lastStatus.remoteAudioPayloadInfo;
	}

	virtual QList<PPayloadInfo> remoteVideoPayloadInfo() const
	{
		return lastStatus.remoteVideoPayloadInfo;
	}

	virtual QList<PAudioParams> audioParams() const
	{
		return lastStatus.localAudioParams;
	}

	virtual QList<PVideoParams> videoParams() const
	{
		return lastStatus.localVideoParams;
	}

	virtual bool canTransmitAudio() const
	{
		return lastStatus.canTransmitAudio;
	}

	virtual bool canTransmitVideo() const
	{
		return lastStatus.canTransmitVideo;
	}

	virtual int outputVolume() const
	{
		return devices.audioOutVolume;
	}

	virtual void setOutputVolume(int level)
	{
		devices.audioOutVolume = level;
		if(control)
			control->updateDevices(devices);
	}

	virtual int inputVolume() const
	{
		return devices.audioInVolume;
	}

	virtual void setInputVolume(int level)
	{
		devices.audioInVolume = level;
		if(control)
			control->updateDevices(devices);
	}

	virtual Error errorCode() const
	{
		return (Error)lastStatus.errorCode;
	}

	virtual RtpChannelContext *audioRtpChannel()
	{
		return &audioRtp;
	}

	virtual RtpChannelContext *videoRtpChannel()
	{
		return &videoRtp;
	}

	// channel calls this, which may be in another thread
	void push_packet_for_write(GstRtpChannel *from, const PRtpPacket &rtp)
	{
		QMutexLocker locker(&write_mutex);
		if(!allow_writes || !control)
			return;

		if(from == &audioRtp)
			control->rtpAudioIn(rtp);
		else if(from == &videoRtp)
			control->rtpVideoIn(rtp);
	}

signals:
	void started();
	void preferencesUpdated();
	void audioOutputIntensityChanged(int intensity);
	void audioInputIntensityChanged(int intensity);
	void stoppedRecording();
	void stopped();
	void finished();
	void error();

private slots:
	void control_statusReady(const RwControlStatus &status)
	{
		lastStatus = status;

		if(status.finished)
		{
			// finished status just means the file is done
			//   sending.  the session still remains active.
			emit finished();
		}
		else if(status.error)
		{
			cleanup();
			emit error();
		}
		else if(pending_status)
		{
			if(status.stopped)
			{
				pending_status = false;

				cleanup();
				emit stopped();
				return;
			}

			// if we're currently stopping, ignore all other
			//   pending status events except for stopped
			//   (handled above)
			if(isStopping)
				return;

			pending_status = false;

			if(!isStarted)
			{
				isStarted = true;

				// if there was a pending record, start it
				recorder.startNext();

				emit started();
			}
			else
				emit preferencesUpdated();
		}
	}

	void control_previewFrame(const QImage &img)
	{
		if(previewWidget)
			previewWidget->show_frame(img);
	}

	void control_outputFrame(const QImage &img)
	{
		if(outputWidget)
			outputWidget->show_frame(img);
	}

	void control_audioOutputIntensityChanged(int intensity)
	{
		emit audioOutputIntensityChanged(intensity);
	}

	void control_audioInputIntensityChanged(int intensity)
	{
		emit audioInputIntensityChanged(intensity);
	}

	void recorder_stopped()
	{
		emit stoppedRecording();
	}

private:
	static void cb_control_rtpAudioOut(const PRtpPacket &packet, void *app)
	{
		((GstRtpSessionContext *)app)->control_rtpAudioOut(packet);
	}

	static void cb_control_rtpVideoOut(const PRtpPacket &packet, void *app)
	{
		((GstRtpSessionContext *)app)->control_rtpVideoOut(packet);
	}

	static void cb_control_recordData(const QByteArray &packet, void *app)
	{
		((GstRtpSessionContext *)app)->control_recordData(packet);
	}

	// note: this is executed from a different thread
	void control_rtpAudioOut(const PRtpPacket &packet)
	{
		audioRtp.push_packet_for_read(packet);
	}

	// note: this is executed from a different thread
	void control_rtpVideoOut(const PRtpPacket &packet)
	{
		videoRtp.push_packet_for_read(packet);
	}

	// note: this is executed from a different thread
	void control_recordData(const QByteArray &packet)
	{
		recorder.push_data_for_read(packet);
	}
};

void GstRtpChannel::receiver_push_packet_for_write(const PRtpPacket &rtp)
{
	if(session)
		session->push_packet_for_write(this, rtp);
}

//----------------------------------------------------------------------------
// GstProvider
//----------------------------------------------------------------------------
class GstProvider : public QObject, public Provider
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::Provider)

public:
	GstThread *thread;

	GstProvider() :
		thread(0)
	{
	}

	virtual QObject *qobject()
	{
		return this;
	}

	virtual bool init(const QString &resourcePath)
	{
		thread = new GstThread(this);
		if(!thread->start(resourcePath))
		{
			delete thread;
			thread = 0;
			return false;
		}

		return true;
	}

	~GstProvider()
	{
		delete thread;
	}

	virtual QString creditName()
	{
		return "GStreamer";
	}

	virtual QString creditText()
	{
		QString str = QString(
		"This application uses GStreamer %1, a comprehensive "
		"open-source and cross-platform multimedia framework.  For "
		"more information, see http://www.gstreamer.net/\n\n"
		"If you enjoy this software, please give the GStreamer "
		"people a million dollars."
		).arg(thread->gstVersion());
		return str;
	}

	virtual FeaturesContext *createFeatures()
	{
		return new GstFeaturesContext(thread);
	}

	virtual RtpSessionContext *createRtpSession()
	{
		return new GstRtpSessionContext(thread);
	}
};

class GstPlugin : public QObject, public Plugin
{
	Q_OBJECT
	Q_INTERFACES(PsiMedia::Plugin)

public:
	virtual Provider *createProvider() { return new GstProvider; }
};

}

Q_EXPORT_PLUGIN2(gstprovider, PsiMedia::GstPlugin)

#include "gstprovider.moc"
