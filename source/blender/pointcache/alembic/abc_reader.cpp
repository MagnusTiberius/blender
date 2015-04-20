/*
 * Copyright 2013, Blender Foundation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

//#include <Alembic/AbcCoreHDF5/ReadWrite.h>
#include <Alembic/AbcCoreOgawa/ReadWrite.h>
#include <Alembic/Abc/ArchiveInfo.h>
#include <Alembic/Abc/IArchive.h>
#include <Alembic/Abc/IObject.h>

#include "alembic.h"
#include "abc_reader.h"

#include "util_error_handler.h"

extern "C" {
#include "DNA_scene_types.h"
}

namespace PTC {

using namespace Abc;

AbcReaderArchive *AbcReaderArchive::open(Scene *scene, const std::string &filename, ErrorHandler *error_handler)
{
	IArchive abc_archive;
	PTC_SAFE_CALL_BEGIN
//	abc_archive = IArchive(AbcCoreHDF5::ReadArchive(), filename, Abc::ErrorHandler::kThrowPolicy);
	abc_archive = IArchive(AbcCoreOgawa::ReadArchive(), filename, Abc::ErrorHandler::kThrowPolicy);
	PTC_SAFE_CALL_END_HANDLER(error_handler)
	
	if (abc_archive)
		return new AbcReaderArchive(scene, error_handler, abc_archive);
	else
		return NULL;
}

AbcReaderArchive::AbcReaderArchive(Scene *scene, ErrorHandler *error_handler, IArchive abc_archive) :
    FrameMapper(scene),
    m_error_handler(error_handler),
    m_use_render(false),
    m_abc_archive(abc_archive)
{
	m_abc_root = IObject(m_abc_archive.getTop(), "root");
	m_abc_root_render = IObject(m_abc_archive.getTop(), "root_render");
}

AbcReaderArchive::~AbcReaderArchive()
{
}

Abc::IObject AbcReaderArchive::root()
{
	if (m_use_render)
		return m_abc_root_render;
	else
		return m_abc_root;
}

IObject AbcReaderArchive::get_id_object(ID *id)
{
	if (!m_abc_archive)
		return IObject();
	
	IObject root = this->root();
	return root.getChild(id->name);
}

bool AbcReaderArchive::has_id_object(ID *id)
{
	if (!m_abc_archive)
		return false;
	
	IObject root = this->root();
	return root.getChild(id->name).valid();
}

bool AbcReaderArchive::get_frame_range(int &start_frame, int &end_frame)
{
	if (m_abc_archive) {
		double start_time, end_time;
		GetArchiveStartAndEndTime(m_abc_archive, start_time, end_time);
		start_frame = (int)time_to_frame(start_time);
		end_frame = (int)time_to_frame(end_time);
		return true;
	}
	else {
		start_frame = end_frame = 1;
		return false;
	}
}

void AbcReaderArchive::get_info(void (*stream)(void *, const char *), void *userdata)
{
	if (m_abc_archive)
		abc_archive_info(m_abc_archive, stream, userdata);
}

ISampleSelector AbcReaderArchive::get_frame_sample_selector(float frame)
{
	return ISampleSelector(frame_to_time(frame), ISampleSelector::kFloorIndex);
}


bool AbcReader::get_frame_range(int &start_frame, int &end_frame)
{
	return m_abc_archive->get_frame_range(start_frame, end_frame);
}

PTCReadSampleResult AbcReader::test_sample(float frame)
{
	if (m_abc_archive) {
		int start_frame, end_frame;
		m_abc_archive->get_frame_range(start_frame, end_frame);
		
		if (frame < start_frame)
			return PTC_READ_SAMPLE_EARLY;
		else if (frame > end_frame)
			return PTC_READ_SAMPLE_LATE;
		else {
			/* TODO could also be EXACT, but INTERPOLATED is more general
				 * do we need to support this?
				 * checking individual time samplings is also possible, but more involved.
				 */
			return PTC_READ_SAMPLE_INTERPOLATED;
		}
	}
	else {
		return PTC_READ_SAMPLE_INVALID;
	}
}

} /* namespace PTC */