/*
 * Copyright (C) 2009  Barracuda Networks, Inc.
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

#ifndef PSI_PIPELINE_H
#define PSI_PIPELINE_H

#include <QString>
#include <gst/gstelement.h>
#include "psimediaprovider.h"

namespace PsiMedia {

class PipelineDeviceContext;
class PipelineDeviceContextPrivate;

class PipelineContext
{
public:
	PipelineContext();
	~PipelineContext();

	GstElement *pipelineElement();

private:
	friend class PipelineDeviceContext;
	friend class PipelineDeviceContextPrivate;

	class Private;
	Private *d;
};

// this is for hinting video input properties.  the actual video quality may
//   end up being something else, but in general it will try to be the closest
//   possible quality to satisfy the maximum hinted of all the references.
//   thus, if one ref is made, set to 640x480, and another ref is made, set to
//   320x240, the quality generated by the device (and therefore, both refs)
//   will probably be 640x480.
class PipelineDeviceOptions
{
public:
	QSize videoSize;
	int fps;

	PipelineDeviceOptions() :
		fps(-1)
	{
	}
};

class PipelineDeviceContext
{
public:
	PipelineDeviceContext(PipelineContext *pipeline, const QString &id, PDevice::Type type, const PipelineDeviceOptions &opts = PipelineDeviceOptions());
	~PipelineDeviceContext();

	GstElement *deviceElement();

	void setOptions(const PipelineDeviceOptions &opts);

private:
	PipelineDeviceContextPrivate *d;
};

}

#endif
