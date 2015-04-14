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

#include "abc_cloth.h"
#include "abc_mesh.h"
#include "abc_particles.h"

extern "C" {
#include "BLI_listbase.h"
#include "BLI_math.h"

#include "DNA_listBase.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_particle_types.h"

#include "BKE_anim.h"
#include "BKE_particle.h"
#include "BKE_strands.h"
}

namespace PTC {

using namespace Abc;
using namespace AbcGeom;

AbcParticlesWriter::AbcParticlesWriter(const std::string &name, Object *ob, ParticleSystem *psys) :
    ParticlesWriter(ob, psys, name)
{
}

AbcParticlesWriter::~AbcParticlesWriter()
{
}

void AbcParticlesWriter::init_abc(OObject parent)
{
	if (m_points)
		return;
	m_points = OPoints(parent, m_name, abc_archive()->frame_sampling_index());
}

void AbcParticlesWriter::write_sample()
{
	if (!m_points)
		return;
	
	OPointsSchema &schema = m_points.getSchema();
	
	int totpart = m_psys->totpart;
	ParticleData *pa;
	int i;
	
	/* XXX TODO only needed for the first frame/sample */
	std::vector<Util::uint64_t> ids;
	ids.reserve(totpart);
	for (i = 0, pa = m_psys->particles; i < totpart; ++i, ++pa)
		ids.push_back(i);
	
	std::vector<V3f> positions;
	positions.reserve(totpart);
	for (i = 0, pa = m_psys->particles; i < totpart; ++i, ++pa) {
		float *co = pa->state.co;
		positions.push_back(V3f(co[0], co[1], co[2]));
	}
	
	OPointsSchema::Sample sample = OPointsSchema::Sample(V3fArraySample(positions), UInt64ArraySample(ids));

	schema.set(sample);
}


AbcParticlesReader::AbcParticlesReader(const std::string &name, Object *ob, ParticleSystem *psys) :
    ParticlesReader(ob, psys, name)
{
}

AbcParticlesReader::~AbcParticlesReader()
{
}

void AbcParticlesReader::init_abc(IObject object)
{
	if (m_points)
		return;
	m_points = IPoints(object, kWrapExisting);
	
	/* XXX TODO read first sample for info on particle count and times */
	m_totpoint = 0;
}

PTCReadSampleResult AbcParticlesReader::read_sample(float frame)
{
	ISampleSelector ss = abc_archive()->get_frame_sample_selector(frame);
	
	if (!m_points.valid())
		return PTC_READ_SAMPLE_INVALID;
	
	IPointsSchema &schema = m_points.getSchema();
	if (schema.getNumSamples() == 0)
		return PTC_READ_SAMPLE_INVALID;
	
	IPointsSchema::Sample sample;
	schema.get(sample, ss);
	
	const V3f *positions = sample.getPositions()->get();
	int /*totpart = m_psys->totpart,*/ i;
	ParticleData *pa;
	for (i = 0, pa = m_psys->particles; i < sample.getPositions()->size(); ++i, ++pa) {
		pa->state.co[0] = positions[i].x;
		pa->state.co[1] = positions[i].y;
		pa->state.co[2] = positions[i].z;
	}
	
	return PTC_READ_SAMPLE_EXACT;
}


struct StrandsChildrenSample {
	std::vector<int32_t> numverts;
	std::vector<M33f> root_matrix;
	std::vector<V3f> root_positions;
	
	std::vector<V3f> positions;
	std::vector<float32_t> times;
	std::vector<int32_t> parents;
	std::vector<float32_t> parent_weights;
};

struct StrandsSample {
	std::vector<int32_t> numverts;
	std::vector<M33f> root_matrix;
	
	std::vector<V3f> positions;
	std::vector<float32_t> times;
	std::vector<float32_t> weights;
	
	std::vector<V3f> motion_co;
	std::vector<V3f> motion_vel;
};

AbcHairChildrenWriter::AbcHairChildrenWriter(const std::string &name, Object *ob, ParticleSystem *psys) :
    ParticlesWriter(ob, psys, name)
{
	m_psmd = psys_get_modifier(ob, psys);
}

AbcHairChildrenWriter::~AbcHairChildrenWriter()
{
}

void AbcHairChildrenWriter::init_abc(OObject parent)
{
	if (m_curves)
		return;
	
	/* XXX non-escaped string construction here ... */
	m_curves = OCurves(parent, m_name, abc_archive()->frame_sampling_index());
	
	OCurvesSchema &schema = m_curves.getSchema();
	OCompoundProperty geom_props = schema.getArbGeomParams();
	OCompoundProperty user_props = schema.getUserProperties();
	
	m_prop_root_matrix = OM33fArrayProperty(user_props, "root_matrix", abc_archive()->frame_sampling());
	m_prop_root_positions = OV3fArrayProperty(user_props, "root_positions", abc_archive()->frame_sampling());
	m_param_times = OFloatGeomParam(geom_props, "times", false, kVertexScope, 1, 0);
	m_prop_parents = OInt32ArrayProperty(user_props, "parents", abc_archive()->frame_sampling());
	m_prop_parent_weights = OFloatArrayProperty(user_props, "parent_weights", abc_archive()->frame_sampling());
}

static int hair_children_count_totkeys(ParticleCacheKey **pathcache, int totpart)
{
	int p;
	int totkeys = 0;
	
	if (pathcache) {
		for (p = 0; p < totpart; ++p) {
			ParticleCacheKey *keys = pathcache[p];
			totkeys += keys->segments + 1;
		}
	}
	
	return totkeys;
}

#if 0
static int hair_children_parent_advance(HairKey *keys, int totkeys, float time, int k)
{
	for (; k + 1 < totkeys; ++k) {
		if (keys[k+1].time > time)
			break;
	}
	return k;
}

static void hair_children_calc_strand(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, ChildParticle *cpa, ParticleCacheKey *keys, int maxkeys, StrandsChildrenSample &sample)
{
	const bool between = (psys->part->childtype == PART_CHILD_FACES);
	ParticleData *parent[4];
	float weight[4];
	float hairmat[4][4][4];
	int parent_key[4] = {0,0,0,0};
	
	int i, k;
	
	if (between) {
		for (i = 0; i < 4; ++i) {
			parent[i] = &psys->particles[cpa->pa[i]];
			weight[i] = cpa->w[i];
			if (parent[i])
				psys_mat_hair_to_global(ob, psmd->dm, psys->part->from, parent[i], hairmat[i]);
		}
	}
	else {
		parent[0] = &psys->particles[cpa->parent];
		parent[1] = parent[2] = parent[3] = NULL;
		weight[0] = 1.0f;
		weight[1] = weight[2] = weight[3] = 0.0f;
		if (parent[0])
			psys_mat_hair_to_global(ob, psmd->dm, psys->part->from, parent[0], hairmat[0]);
	}
	
	int numkeys = keys->segments + 1;
	for (k = 0; k < numkeys; ++k) {
		ParticleCacheKey *key = &keys[k];
		/* XXX particle time values are too messy and confusing, recalculate */
		float time = maxkeys > 1 ? (float)k / (float)(maxkeys-1) : 0.0f;
		
		float parent_co[3];
		zero_v3(parent_co);
		for (i = 0; i < 4; ++i) {
			if (!parent[i] || weight[i] <= 0.0f)
				continue;
			parent_key[i] = hair_children_parent_advance(parent[i]->hair, parent[i]->totkey, time, parent_key[i]);
			
			float key_co[3];
			if (parent_key[i] + 1 < parent[i]->totkey) {
				HairKey *key0 = &parent[i]->hair[parent_key[i]];
				HairKey *key1 = &parent[i]->hair[parent_key[i] + 1];
				float x = (key1->time > key0->time) ? (time - key0->time) / (key1->time - key0->time) : 0.0f;
				interp_v3_v3v3(key_co, key0->co, key1->co, x);
			}
			else {
				HairKey *key0 = &parent[i]->hair[parent_key[i]];
				copy_v3_v3(key_co, key0->co);
			}
			
			madd_v3_v3fl(parent_co, key_co, weight[i]);
			
			/* Hair keys are in hair root space, pathcache keys are in world space,
			 * transform both to world space to calculate the offset
			 */
			mul_m4_v3(hairmat[i], parent_co);
		}
		
		/* child position is an offset from the parent */
		float co[3];
		sub_v3_v3v3(co, key->co, parent_co);
		
		sample.positions.push_back(V3f(parent_co[0], parent_co[1], parent_co[2]));
		sample.times.push_back(time);
	}
}
#endif

static void hair_children_create_sample(Object *ob, ParticleSystem *psys, ParticleSystemModifierData *psmd, ParticleCacheKey **pathcache, int totpart, int totkeys, int maxkeys,
                                        StrandsChildrenSample &sample, bool write_constants)
{
	const bool between = (psys->part->childtype == PART_CHILD_FACES);
	
	int p, k;
	
	if (write_constants) {
		sample.numverts.reserve(totpart);
		sample.parents.reserve(4*totpart);
		sample.parent_weights.reserve(4*totpart);
		
		sample.positions.reserve(totkeys);
		sample.times.reserve(totkeys);
	}
	
	sample.root_matrix.reserve(totpart);
	sample.root_positions.reserve(totpart);
	
	for (p = 0; p < totpart; ++p) {
		ChildParticle *cpa = &psys->child[p];
		
		float hairmat[4][4];
		psys_child_mat_to_object(ob, psys, psmd, cpa, hairmat);
		
		if (pathcache) {
			ParticleCacheKey *keys = pathcache[p];
			int numkeys = keys->segments + 1;
			
			if (write_constants) {
				sample.numverts.push_back(numkeys);
				if (between) {
					sample.parents.push_back(cpa->pa[0]);
					sample.parents.push_back(cpa->pa[1]);
					sample.parents.push_back(cpa->pa[2]);
					sample.parents.push_back(cpa->pa[3]);
					sample.parent_weights.push_back(cpa->w[0]);
					sample.parent_weights.push_back(cpa->w[1]);
					sample.parent_weights.push_back(cpa->w[2]);
					sample.parent_weights.push_back(cpa->w[3]);
				}
				else {
					sample.parents.push_back(cpa->parent);
					sample.parents.push_back(-1);
					sample.parents.push_back(-1);
					sample.parents.push_back(-1);
					sample.parent_weights.push_back(1.0f);
					sample.parent_weights.push_back(0.0f);
					sample.parent_weights.push_back(0.0f);
					sample.parent_weights.push_back(0.0f);
				}
				
				float imat[4][4];
				mul_m4_m4m4(imat, ob->obmat, hairmat);
				invert_m4(imat);
				
				for (k = 0; k < numkeys; ++k) {
					ParticleCacheKey *key = &keys[k];
					float co[3];
					/* pathcache keys are in world space, transform to hair root space */
					mul_v3_m4v3(co, imat, key->co);
					
					sample.positions.push_back(V3f(co[0], co[1], co[2]));
					/* XXX particle time values are too messy and confusing, recalculate */
					sample.times.push_back(maxkeys > 1 ? (float)k / (float)(maxkeys-1) : 0.0f);
				}
			}
		}
		
		float mat3[3][3];
		copy_m3_m4(mat3, hairmat);
		sample.root_matrix.push_back(M33f(mat3));
		float *co = hairmat[3];
		sample.root_positions.push_back(V3f(co[0], co[1], co[2]));
	}
}

void AbcHairChildrenWriter::write_sample()
{
	if (!m_curves)
		return;
	
	int totkeys = hair_children_count_totkeys(m_psys->childcache, m_psys->totchild);
	
	int keysteps = abc_archive()->use_render() ? m_psys->part->ren_step : m_psys->part->draw_step;
	int maxkeys = (1 << keysteps) + 1 + (m_psys->part->kink);
	if (ELEM(m_psys->part->kink, PART_KINK_SPIRAL))
		maxkeys += m_psys->part->kink_extra_steps;
	
	OCurvesSchema &schema = m_curves.getSchema();
	
	StrandsChildrenSample child_sample;
	OCurvesSchema::Sample sample;
	if (schema.getNumSamples() == 0) {
		/* write curve sizes only first time, assuming they are constant! */
		hair_children_create_sample(m_ob, m_psys, m_psmd, m_psys->childcache, m_psys->totchild, totkeys, maxkeys, child_sample, true);
		sample = OCurvesSchema::Sample(child_sample.positions, child_sample.numverts);
		
		m_prop_parents.set(Int32ArraySample(child_sample.parents));
		m_prop_parent_weights.set(FloatArraySample(child_sample.parent_weights));
		
		m_param_times.set(OFloatGeomParam::Sample(FloatArraySample(child_sample.times), kVertexScope));
		
		schema.set(sample);
	}
	else {
		hair_children_create_sample(m_ob, m_psys, m_psmd, m_psys->childcache, m_psys->totchild, totkeys, maxkeys, child_sample, false);
	}
	
	m_prop_root_matrix.set(M33fArraySample(child_sample.root_matrix));
	m_prop_root_positions.set(V3fArraySample(child_sample.root_positions));
}


AbcHairWriter::AbcHairWriter(const std::string &name, Object *ob, ParticleSystem *psys) :
    ParticlesWriter(ob, psys, name),
    m_child_writer("children", ob, psys)
{
	m_psmd = psys_get_modifier(ob, psys);
}

AbcHairWriter::~AbcHairWriter()
{
}

void AbcHairWriter::init(WriterArchive *archive)
{
	AbcWriter::init(archive);
	m_child_writer.init(archive);
}

void AbcHairWriter::init_abc(OObject parent)
{
	if (m_curves)
		return;
	m_curves = OCurves(parent, m_name, abc_archive()->frame_sampling_index());
	
	OCurvesSchema &schema = m_curves.getSchema();
	OCompoundProperty geom_props = schema.getArbGeomParams();
	
	m_param_root_matrix = OM33fGeomParam(geom_props, "root_matrix", false, kUniformScope, 1, 0);
	
	m_param_times = OFloatGeomParam(geom_props, "times", false, kVertexScope, 1, 0);
	m_param_weights = OFloatGeomParam(geom_props, "weights", false, kVertexScope, 1, 0);
	
	m_child_writer.init_abc(m_curves);
}

static int hair_count_totverts(ParticleSystem *psys)
{
	int p;
	int totverts = 0;
	
	for (p = 0; p < psys->totpart; ++p) {
		ParticleData *pa = &psys->particles[p];
		totverts += pa->totkey;
	}
	
	return totverts;
}

static void hair_create_sample(Object *ob, DerivedMesh *dm, ParticleSystem *psys, StrandsSample &sample, bool do_numverts)
{
	int totpart = psys->totpart;
	int totverts = hair_count_totverts(psys);
	int p, k;
	
	if (totverts == 0)
		return;
	
	if (do_numverts)
		sample.numverts.reserve(totpart);
	sample.root_matrix.reserve(totpart);
	sample.positions.reserve(totverts);
	sample.times.reserve(totverts);
	sample.weights.reserve(totverts);
	
	for (p = 0; p < totpart; ++p) {
		ParticleData *pa = &psys->particles[p];
		int numverts = pa->totkey;
		float hairmat[4][4], root_matrix[3][3];
		
		if (do_numverts)
			sample.numverts.push_back(numverts);
		
		psys_mat_hair_to_object(ob, dm, psys->part->from, pa, hairmat);
		copy_m3_m4(root_matrix, hairmat);
		sample.root_matrix.push_back(M33f(root_matrix));
		
		for (k = 0; k < numverts; ++k) {
			HairKey *key = &pa->hair[k];
			float hairmat[4][4];
			float co[3];
			
			/* hair keys are in "hair space" relative to the mesh,
			 * store them in object space for compatibility and to avoid
			 * complexities of how particles work.
			 */
			psys_mat_hair_to_object(ob, dm, psys->part->from, pa, hairmat);
			mul_v3_m4v3(co, hairmat, key->co);
			
			sample.positions.push_back(V3f(co[0], co[1], co[2]));
			/* XXX particle time values are too messy and confusing, recalculate */
			sample.times.push_back(numverts > 1 ? (float)k / (float)(numverts-1) : 0.0f);
			sample.weights.push_back(key->weight);
		}
	}
}

void AbcHairWriter::write_sample()
{
	if (!m_curves)
		return;
	if (!m_psmd || !m_psmd->dm)
		return;
	
	OCurvesSchema &schema = m_curves.getSchema();
	
	StrandsSample hair_sample;
	OCurvesSchema::Sample sample;
	if (schema.getNumSamples() == 0) {
		/* write curve sizes only first time, assuming they are constant! */
		hair_create_sample(m_ob, m_psmd->dm, m_psys, hair_sample, true);
		sample = OCurvesSchema::Sample(hair_sample.positions, hair_sample.numverts);
	}
	else {
		hair_create_sample(m_ob, m_psmd->dm, m_psys, hair_sample, false);
		sample = OCurvesSchema::Sample(hair_sample.positions);
	}
	schema.set(sample);
	
	m_param_root_matrix.set(OM33fGeomParam::Sample(M33fArraySample(hair_sample.root_matrix), kUniformScope));
	
	m_param_times.set(OFloatGeomParam::Sample(FloatArraySample(hair_sample.times), kVertexScope));
	m_param_weights.set(OFloatGeomParam::Sample(FloatArraySample(hair_sample.weights), kVertexScope));
	
	m_child_writer.write_sample();
}


AbcStrandsChildrenWriter::AbcStrandsChildrenWriter(const std::string &name, const std::string &abc_name, DupliObjectData *dobdata) :
    m_name(name),
    m_abc_name(abc_name),
    m_dobdata(dobdata)
{
}

StrandsChildren *AbcStrandsChildrenWriter::get_strands() const
{
	return BKE_dupli_object_data_find_strands_children(m_dobdata, m_name.c_str());
}

void AbcStrandsChildrenWriter::init_abc(OObject parent)
{
	if (m_curves)
		return;
	m_curves = OCurves(parent, m_abc_name, abc_archive()->frame_sampling_index());
	
	OCurvesSchema &schema = m_curves.getSchema();
	OCompoundProperty geom_props = schema.getArbGeomParams();
	OCompoundProperty user_props = schema.getUserProperties();
	
	m_prop_root_matrix = OM33fArrayProperty(user_props, "root_matrix", abc_archive()->frame_sampling());
	m_prop_root_positions = OV3fArrayProperty(user_props, "root_positions", abc_archive()->frame_sampling());
	m_param_times = OFloatGeomParam(geom_props, "times", false, kVertexScope, 1, abc_archive()->frame_sampling());
	m_prop_parents = OInt32ArrayProperty(user_props, "parents", abc_archive()->frame_sampling());
	m_prop_parent_weights = OFloatArrayProperty(user_props, "parent_weights", abc_archive()->frame_sampling());
}

static void strands_children_create_sample(StrandsChildren *strands, StrandsChildrenSample &sample, bool write_constants)
{
	int totcurves = strands->totcurves;
	int totverts = strands->totverts;
	
	if (write_constants) {
		sample.numverts.reserve(totcurves);
		sample.parents.reserve(4*totcurves);
		sample.parent_weights.reserve(4*totcurves);
		
		sample.positions.reserve(totverts);
		sample.times.reserve(totverts);
	}
	
	sample.root_matrix.reserve(totcurves);
	sample.root_positions.reserve(totcurves);
	
	StrandChildIterator it_strand;
	for (BKE_strand_child_iter_init(&it_strand, strands); BKE_strand_child_iter_valid(&it_strand); BKE_strand_child_iter_next(&it_strand)) {
		int numverts = it_strand.curve->numverts;
		
		if (write_constants) {
			sample.numverts.push_back(numverts);
			
			sample.parents.push_back(it_strand.curve->parents[0]);
			sample.parents.push_back(it_strand.curve->parents[1]);
			sample.parents.push_back(it_strand.curve->parents[2]);
			sample.parents.push_back(it_strand.curve->parents[3]);
			sample.parent_weights.push_back(it_strand.curve->parent_weights[0]);
			sample.parent_weights.push_back(it_strand.curve->parent_weights[1]);
			sample.parent_weights.push_back(it_strand.curve->parent_weights[2]);
			sample.parent_weights.push_back(it_strand.curve->parent_weights[3]);
			
			StrandChildVertexIterator it_vert;
			for (BKE_strand_child_vertex_iter_init(&it_vert, &it_strand); BKE_strand_child_vertex_iter_valid(&it_vert); BKE_strand_child_vertex_iter_next(&it_vert)) {
				const float *co = it_vert.vertex->co;
				sample.positions.push_back(V3f(co[0], co[1], co[2]));
				sample.times.push_back(it_vert.vertex->time);
			}
		}
		
		float mat3[3][3];
		copy_m3_m4(mat3, it_strand.curve->root_matrix);
		sample.root_matrix.push_back(M33f(mat3));
		float *co = it_strand.curve->root_matrix[3];
		sample.root_positions.push_back(V3f(co[0], co[1], co[2]));
	}
}

void AbcStrandsChildrenWriter::write_sample()
{
	if (!m_curves)
		return;
	StrandsChildren *strands = get_strands();
	if (!strands)
		return;
	
	OCurvesSchema &schema = m_curves.getSchema();
	
	StrandsChildrenSample strands_sample;
	OCurvesSchema::Sample sample;
	if (schema.getNumSamples() == 0) {
		/* write curve sizes only first time, assuming they are constant! */
		strands_children_create_sample(strands, strands_sample, true);
		sample = OCurvesSchema::Sample(strands_sample.positions, strands_sample.numverts);
		
		m_prop_parents.set(Int32ArraySample(strands_sample.parents));
		m_prop_parent_weights.set(FloatArraySample(strands_sample.parent_weights));
		
		m_param_times.set(OFloatGeomParam::Sample(FloatArraySample(strands_sample.times), kVertexScope));
		
		schema.set(sample);
	}
	else {
		strands_children_create_sample(strands, strands_sample, false);
	}
	
	m_prop_root_matrix.set(M33fArraySample(strands_sample.root_matrix));
	m_prop_root_positions.set(V3fArraySample(strands_sample.root_positions));
}


AbcStrandsWriter::AbcStrandsWriter(const std::string &name, DupliObjectData *dobdata) :
    m_name(name),
    m_dobdata(dobdata),
    m_child_writer(name, "children", dobdata)
{
}

Strands *AbcStrandsWriter::get_strands() const
{
	return BKE_dupli_object_data_find_strands(m_dobdata, m_name.c_str());
}

void AbcStrandsWriter::init(WriterArchive *archive)
{
	AbcWriter::init(archive);
	m_child_writer.init(archive);
}

void AbcStrandsWriter::init_abc(OObject parent)
{
	if (m_curves)
		return;
	m_curves = OCurves(parent, m_name, abc_archive()->frame_sampling_index());
	
	OCurvesSchema &schema = m_curves.getSchema();
	OCompoundProperty geom_props = schema.getArbGeomParams();
	
	m_param_root_matrix = OM33fGeomParam(geom_props, "root_matrix", false, kUniformScope, 1, abc_archive()->frame_sampling());
	
	m_param_times = OFloatGeomParam(geom_props, "times", false, kVertexScope, 1, abc_archive()->frame_sampling());
	m_param_weights = OFloatGeomParam(geom_props, "weights", false, kVertexScope, 1, abc_archive()->frame_sampling());
	
	m_param_motion_state = OCompoundProperty(geom_props, "motion_state", abc_archive()->frame_sampling());
	m_param_motion_co = OP3fGeomParam(m_param_motion_state, "position", false, kVertexScope, 1, abc_archive()->frame_sampling());
	m_param_motion_vel = OV3fGeomParam(m_param_motion_state, "velocity", false, kVertexScope, 1, abc_archive()->frame_sampling());
	
	m_child_writer.init_abc(m_curves);
}

static void strands_create_sample(Strands *strands, StrandsSample &sample, bool do_numverts)
{
	const bool do_state = strands->state;
	
	int totcurves = strands->totcurves;
	int totverts = strands->totverts;
	
	if (totverts == 0)
		return;
	
	if (do_numverts)
		sample.numverts.reserve(totcurves);
	sample.root_matrix.reserve(totcurves);
	
	sample.positions.reserve(totverts);
	sample.times.reserve(totverts);
	sample.weights.reserve(totverts);
	if (do_state) {
		sample.motion_co.reserve(totverts);
		sample.motion_vel.reserve(totverts);
	}
	
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		int numverts = it_strand.curve->numverts;
		
		if (do_numverts)
			sample.numverts.push_back(numverts);
		sample.root_matrix.push_back(M33f(it_strand.curve->root_matrix));
		
		StrandVertexIterator it_vert;
		for (BKE_strand_vertex_iter_init(&it_vert, &it_strand); BKE_strand_vertex_iter_valid(&it_vert); BKE_strand_vertex_iter_next(&it_vert)) {
			const float *co = it_vert.vertex->co;
			sample.positions.push_back(V3f(co[0], co[1], co[2]));
			sample.times.push_back(it_vert.vertex->time);
			sample.weights.push_back(it_vert.vertex->weight);
			
			if (do_state) {
				float *co = it_vert.state->co;
				float *vel = it_vert.state->vel;
				sample.motion_co.push_back(V3f(co[0], co[1], co[2]));
				sample.motion_vel.push_back(V3f(vel[0], vel[1], vel[2]));
			}
		}
	}
}

void AbcStrandsWriter::write_sample()
{
	if (!m_curves)
		return;
	Strands *strands = get_strands();
	if (!strands)
		return;
	
	OCurvesSchema &schema = m_curves.getSchema();
	
	StrandsSample strands_sample;
	OCurvesSchema::Sample sample;
	if (schema.getNumSamples() == 0) {
		/* write curve sizes only first time, assuming they are constant! */
		strands_create_sample(strands, strands_sample, true);
		sample = OCurvesSchema::Sample(strands_sample.positions, strands_sample.numverts);
	}
	else {
		strands_create_sample(strands, strands_sample, false);
		sample = OCurvesSchema::Sample(strands_sample.positions);
	}
	schema.set(sample);
	
	m_param_root_matrix.set(OM33fGeomParam::Sample(M33fArraySample(strands_sample.root_matrix), kUniformScope));
	
	m_param_times.set(OFloatGeomParam::Sample(FloatArraySample(strands_sample.times), kVertexScope));
	m_param_weights.set(OFloatGeomParam::Sample(FloatArraySample(strands_sample.weights), kVertexScope));
	
	if (strands->state) {
		m_param_motion_co.set(OP3fGeomParam::Sample(P3fArraySample(strands_sample.motion_co), kVertexScope));
		m_param_motion_vel.set(OV3fGeomParam::Sample(V3fArraySample(strands_sample.motion_vel), kVertexScope));
	}
	
	m_child_writer.write_sample();
}


#define PRINT_M3_FORMAT "((%.3f, %.3f, %.3f), (%.3f, %.3f, %.3f), (%.3f, %.3f, %.3f))"
#define PRINT_M3_ARGS(m) (double)m[0][0], (double)m[0][1], (double)m[0][2], (double)m[1][0], (double)m[1][1], (double)m[1][2], (double)m[2][0], (double)m[2][1], (double)m[2][2]
#define PRINT_M4_FORMAT "((%.3f, %.3f, %.3f, %.3f), (%.3f, %.3f, %.3f, %.3f), (%.3f, %.3f, %.3f, %.3f), (%.3f, %.3f, %.3f, %.3f))"
#define PRINT_M4_ARGS(m) (double)m[0][0], (double)m[0][1], (double)m[0][2], (double)m[0][3], (double)m[1][0], (double)m[1][1], (double)m[1][2], (double)m[1][3], \
                         (double)m[2][0], (double)m[2][1], (double)m[2][2], (double)m[2][3], (double)m[3][0], (double)m[3][1], (double)m[3][2], (double)m[3][3]

AbcStrandsChildrenReader::AbcStrandsChildrenReader(StrandsChildren *strands) :
    m_strands(strands)
{
}

AbcStrandsChildrenReader::~AbcStrandsChildrenReader()
{
	discard_result();
}

void AbcStrandsChildrenReader::init_abc(IObject object)
{
	if (m_curves)
		return;
	m_curves = ICurves(object, kWrapExisting);
	
	ICurvesSchema &schema = m_curves.getSchema();
	ICompoundProperty geom_props = schema.getArbGeomParams();
	ICompoundProperty user_props = schema.getUserProperties();
	
	m_prop_root_matrix = IM33fArrayProperty(user_props, "root_matrix");
	m_prop_root_positions = IV3fArrayProperty(user_props, "root_positions");
	m_param_times = IFloatGeomParam(geom_props, "times");
	m_prop_parents = IInt32ArrayProperty(user_props, "parents", 0);
	m_prop_parent_weights = IFloatArrayProperty(user_props, "parent_weights", 0);
}

PTCReadSampleResult AbcStrandsChildrenReader::read_sample(float frame)
{
	ISampleSelector ss = abc_archive()->get_frame_sample_selector(frame);
	
	if (!m_curves.valid())
		return PTC_READ_SAMPLE_INVALID;
	
	ICurvesSchema &schema = m_curves.getSchema();
	if (schema.getNumSamples() == 0)
		return PTC_READ_SAMPLE_INVALID;
	
	ICurvesSchema::Sample sample;
	schema.get(sample, ss);
	
	P3fArraySamplePtr sample_co = sample.getPositions();
	Int32ArraySamplePtr sample_numvert = sample.getCurvesNumVertices();
	M33fArraySamplePtr sample_root_matrix = m_prop_root_matrix.getValue(ss);
	V3fArraySamplePtr sample_root_positions = m_prop_root_positions.getValue(ss);
	IFloatGeomParam::Sample sample_time = m_param_times.getExpandedValue(ss);
	Int32ArraySamplePtr sample_parents = m_prop_parents.getValue(ss);
	FloatArraySamplePtr sample_parent_weights = m_prop_parent_weights.getValue(ss);
	
	if (!sample_co || !sample_numvert)
		return PTC_READ_SAMPLE_INVALID;
	
	int totcurves = sample_numvert->size();
	int totverts = sample_co->size();
	
	if (sample_root_matrix->size() != totcurves ||
	    sample_root_positions->size() != totcurves ||
	    sample_parents->size() != 4 * totcurves ||
	    sample_parent_weights->size() != 4 * totcurves)
		return PTC_READ_SAMPLE_INVALID;
	
	if (m_strands && (m_strands->totcurves != totcurves || m_strands->totverts != totverts))
		m_strands = NULL;
	if (!m_strands)
		m_strands = BKE_strands_children_new(totcurves, totverts);
	
	const int32_t *numvert = sample_numvert->get();
	const M33f *root_matrix = sample_root_matrix->get();
	const V3f *root_positions = sample_root_positions->get();
	const int32_t *parents = sample_parents->get();
	const float32_t *parent_weights = sample_parent_weights->get();
	for (int i = 0; i < sample_numvert->size(); ++i) {
		StrandsChildCurve *scurve = &m_strands->curves[i];
		scurve->numverts = *numvert;
		
		float mat[3][3];
		memcpy(mat, root_matrix->getValue(), sizeof(mat));
		copy_m4_m3(scurve->root_matrix, mat);
		copy_v3_v3(scurve->root_matrix[3], root_positions->getValue());
		
		scurve->parents[0] = parents[0];
		scurve->parents[1] = parents[1];
		scurve->parents[2] = parents[2];
		scurve->parents[3] = parents[3];
		scurve->parent_weights[0] = parent_weights[0];
		scurve->parent_weights[1] = parent_weights[1];
		scurve->parent_weights[2] = parent_weights[2];
		scurve->parent_weights[3] = parent_weights[3];
		
		++numvert;
		++root_matrix;
		++root_positions;
		parents += 4;
		parent_weights += 4;
	}
	
	const V3f *co = sample_co->get();
	const float32_t *time = sample_time.getVals()->get();
	for (int i = 0; i < sample_co->size(); ++i) {
		StrandsChildVertex *svert = &m_strands->verts[i];
		copy_v3_v3(svert->co, co->getValue());
		svert->time = *time;
		
		++co;
		++time;
	}
	
	BKE_strands_children_ensure_normals(m_strands);
	
	return PTC_READ_SAMPLE_EXACT;
}

StrandsChildren *AbcStrandsChildrenReader::acquire_result()
{
	StrandsChildren *strands = m_strands;
	m_strands = NULL;
	return strands;
}

void AbcStrandsChildrenReader::discard_result()
{
	BKE_strands_children_free(m_strands);
	m_strands = NULL;
}


AbcStrandsReader::AbcStrandsReader(Strands *strands, StrandsChildren *children, bool read_motion, bool read_children) :
    m_read_motion(read_motion),
    m_read_children(read_children),
    m_strands(strands),
    m_child_reader(children)
{
}

AbcStrandsReader::~AbcStrandsReader()
{
	discard_result();
}

void AbcStrandsReader::init(ReaderArchive *archive)
{
	AbcReader::init(archive);
	m_child_reader.init(archive);
}

void AbcStrandsReader::init_abc(IObject object)
{
	if (m_curves)
		return;
	m_curves = ICurves(object, kWrapExisting);
	
	ICurvesSchema &schema = m_curves.getSchema();
	ICompoundProperty geom_props = schema.getArbGeomParams();
	
	m_param_root_matrix = IM33fGeomParam(geom_props, "root_matrix");
	
	m_param_times = IFloatGeomParam(geom_props, "times");
	m_param_weights = IFloatGeomParam(geom_props, "weights");
	
	if (m_read_motion && geom_props.getPropertyHeader("motion_state")) {
		m_param_motion_state = ICompoundProperty(geom_props, "motion_state");
		m_param_motion_co = IP3fGeomParam(m_param_motion_state, "position");
		m_param_motion_vel = IV3fGeomParam(m_param_motion_state, "velocity");
	}
	
	if (m_read_children && m_curves.getChildHeader("children")) {
		IObject child = m_curves.getChild("children");
		m_child_reader.init_abc(child);
	}
}

PTCReadSampleResult AbcStrandsReader::read_sample(float frame)
{
	ISampleSelector ss = abc_archive()->get_frame_sample_selector(frame);
	
	if (!m_curves.valid())
		return PTC_READ_SAMPLE_INVALID;
	
	ICurvesSchema &schema = m_curves.getSchema();
	if (schema.getNumSamples() == 0)
		return PTC_READ_SAMPLE_INVALID;
	
	ICurvesSchema::Sample sample, sample_base;
	schema.get(sample, ss);
	schema.get(sample_base, ISampleSelector((index_t)0));
	
	P3fArraySamplePtr sample_co = sample.getPositions();
	P3fArraySamplePtr sample_co_base = sample_base.getPositions();
	Int32ArraySamplePtr sample_numvert = sample.getCurvesNumVertices();
	IM33fGeomParam::Sample sample_root_matrix = m_param_root_matrix.getExpandedValue(ss);
	IFloatGeomParam::Sample sample_time = m_param_times.getExpandedValue(ss);
	IFloatGeomParam::Sample sample_weight = m_param_weights.getExpandedValue(ss);
	
	if (!sample_co || !sample_numvert || !sample_co_base || sample_co_base->size() != sample_co->size())
		return PTC_READ_SAMPLE_INVALID;
	
	if (m_strands && (m_strands->totcurves != sample_numvert->size() || m_strands->totverts != sample_co->size()))
		m_strands = NULL;
	if (!m_strands)
		m_strands = BKE_strands_new(sample_numvert->size(), sample_co->size());
	
	const int32_t *numvert = sample_numvert->get();
	const M33f *root_matrix = sample_root_matrix.getVals()->get();
	for (int i = 0; i < sample_numvert->size(); ++i) {
		StrandsCurve *scurve = &m_strands->curves[i];
		scurve->numverts = *numvert;
		memcpy(scurve->root_matrix, root_matrix->getValue(), sizeof(scurve->root_matrix));
		
		++numvert;
		++root_matrix;
	}
	
	const V3f *co = sample_co->get();
	const V3f *co_base = sample_co_base->get();
	const float32_t *time = sample_time.getVals()->get();
	const float32_t *weight = sample_weight.getVals()->get();
	for (int i = 0; i < sample_co->size(); ++i) {
		StrandsVertex *svert = &m_strands->verts[i];
		copy_v3_v3(svert->co, co->getValue());
		copy_v3_v3(svert->base, co_base->getValue());
		svert->time = *time;
		svert->weight = *weight;
		
		++co;
		++co_base;
		++time;
		++weight;
	}
	
	/* Correction for base coordinates: these are in object space of frame 1,
	 * but we want the relative shape. Offset them to the current root location.
	 */
	StrandIterator it_strand;
	for (BKE_strand_iter_init(&it_strand, m_strands); BKE_strand_iter_valid(&it_strand); BKE_strand_iter_next(&it_strand)) {
		if (it_strand.curve->numverts <= 0)
			continue;
		
		float offset[3];
		sub_v3_v3v3(offset, it_strand.verts[0].co, it_strand.verts[0].base);
		
		StrandVertexIterator it_vert;
		for (BKE_strand_vertex_iter_init(&it_vert, &it_strand); BKE_strand_vertex_iter_valid(&it_vert); BKE_strand_vertex_iter_next(&it_vert)) {
			add_v3_v3(it_vert.vertex->base, offset);
		}
	}
	
	if (m_read_motion &&
	    m_param_motion_co && m_param_motion_co.getNumSamples() > 0 &&
	    m_param_motion_vel && m_param_motion_vel.getNumSamples() > 0)
	{
		IP3fGeomParam::Sample sample_motion_co = m_param_motion_co.getExpandedValue(ss);
		IV3fGeomParam::Sample sample_motion_vel = m_param_motion_vel.getExpandedValue(ss);
		
		const V3f *co = sample_motion_co.getVals()->get();
		const V3f *vel = sample_motion_vel.getVals()->get();
		if (co && vel) {
			BKE_strands_add_motion_state(m_strands);
			
			for (int i = 0; i < m_strands->totverts; ++i) {
				StrandsMotionState *ms = &m_strands->state[i];
				copy_v3_v3(ms->co, co->getValue());
				copy_v3_v3(ms->vel, vel->getValue());
				
				++co;
				++vel;
			}
		}
	}
	
	BKE_strands_ensure_normals(m_strands);
	
	if (m_read_children) {
		m_child_reader.read_sample(frame);
	}
	
	return PTC_READ_SAMPLE_EXACT;
}

Strands *AbcStrandsReader::acquire_result()
{
	Strands *strands = m_strands;
	m_strands = NULL;
	return strands;
}

void AbcStrandsReader::discard_result()
{
	BKE_strands_free(m_strands);
	m_strands = NULL;
}


AbcHairDynamicsWriter::AbcHairDynamicsWriter(const std::string &name, Object *ob, ParticleSystem *psys) :
    ParticlesWriter(ob, psys, name),
    m_cloth_writer(name + "__cloth", ob, psys->clmd)
{
}

void AbcHairDynamicsWriter::init_abc(OObject parent)
{
	m_cloth_writer.init_abc(parent);
}

void AbcHairDynamicsWriter::write_sample()
{
	m_cloth_writer.write_sample();
}

AbcHairDynamicsReader::AbcHairDynamicsReader(const std::string &name, Object *ob, ParticleSystem *psys) :
	ParticlesReader(ob, psys, name),
	m_cloth_reader(name + "__cloth", ob, psys->clmd)
{
}

void AbcHairDynamicsReader::init_abc(IObject object)
{
	m_cloth_reader.init_abc(object);
}

PTCReadSampleResult AbcHairDynamicsReader::read_sample(float frame)
{
	return m_cloth_reader.read_sample(frame);
}


struct ParticlePathcacheSample {
	std::vector<int32_t> numkeys;
	
	std::vector<V3f> positions;
	std::vector<V3f> velocities;
	std::vector<Quatf> rotations;
	std::vector<C3f> colors;
	std::vector<float32_t> times;
};

AbcParticlePathcacheWriter::AbcParticlePathcacheWriter(const std::string &name, Object *ob, ParticleSystem *psys, ParticleCacheKey ***pathcache, int *totpath, const std::string &suffix) :
    ParticlesWriter(ob, psys, name),
    m_pathcache(pathcache),
    m_totpath(totpath),
    m_suffix(suffix)
{
}

AbcParticlePathcacheWriter::~AbcParticlePathcacheWriter()
{
}

void AbcParticlePathcacheWriter::init_abc(OObject parent)
{
	if (m_curves)
		return;
	
	/* XXX non-escaped string construction here ... */
	m_curves = OCurves(parent, m_name + m_suffix, abc_archive()->frame_sampling_index());
	
	OCurvesSchema &schema = m_curves.getSchema();
	OCompoundProperty geom_props = schema.getArbGeomParams();
	
	m_param_velocities = OV3fGeomParam(geom_props, "velocities", false, kVertexScope, 1, 0);
	m_param_rotations = OQuatfGeomParam(geom_props, "rotations", false, kVertexScope, 1, 0);
	m_param_colors = OC3fGeomParam(geom_props, "colors", false, kVertexScope, 1, 0);
	m_param_times = OFloatGeomParam(geom_props, "times", false, kVertexScope, 1, 0);
}

static int paths_count_totkeys(ParticleCacheKey **pathcache, int totpart)
{
	int p;
	int totkeys = 0;
	
	for (p = 0; p < totpart; ++p) {
		ParticleCacheKey *keys = pathcache[p];
		totkeys += keys->segments + 1;
	}
	
	return totkeys;
}

static void paths_create_sample(ParticleCacheKey **pathcache, int totpart, int totkeys, ParticlePathcacheSample &sample, bool do_numkeys)
{
	int p, k;
	
	if (do_numkeys)
		sample.numkeys.reserve(totpart);
	sample.positions.reserve(totkeys);
	sample.velocities.reserve(totkeys);
	sample.rotations.reserve(totkeys);
	sample.colors.reserve(totkeys);
	sample.times.reserve(totkeys);
	
	for (p = 0; p < totpart; ++p) {
		ParticleCacheKey *keys = pathcache[p];
		int numkeys = keys->segments + 1;
		
		if (do_numkeys)
			sample.numkeys.push_back(numkeys);
		
		for (k = 0; k < numkeys; ++k) {
			ParticleCacheKey *key = &keys[k];
			
			sample.positions.push_back(V3f(key->co[0], key->co[1], key->co[2]));
			sample.velocities.push_back(V3f(key->vel[0], key->vel[1], key->vel[2]));
			sample.rotations.push_back(Quatf(key->rot[0], key->rot[1], key->rot[2], key->rot[3]));
			sample.colors.push_back(C3f(key->col[0], key->col[1], key->col[2]));
			sample.times.push_back(key->time);
		}
	}
}

void AbcParticlePathcacheWriter::write_sample()
{
	if (!m_curves)
		return;
	if (!(*m_pathcache))
		return;
	
	int totkeys = paths_count_totkeys(*m_pathcache, *m_totpath);
	if (totkeys == 0)
		return;
	
	OCurvesSchema &schema = m_curves.getSchema();
	
	ParticlePathcacheSample path_sample;
	OCurvesSchema::Sample sample;
	if (schema.getNumSamples() == 0) {
		/* write curve sizes only first time, assuming they are constant! */
		paths_create_sample(*m_pathcache, *m_totpath, totkeys, path_sample, true);
		sample = OCurvesSchema::Sample(path_sample.positions, path_sample.numkeys);
	}
	else {
		paths_create_sample(*m_pathcache, *m_totpath, totkeys, path_sample, false);
		sample = OCurvesSchema::Sample(path_sample.positions);
	}
	schema.set(sample);
	
	m_param_velocities.set(OV3fGeomParam::Sample(V3fArraySample(path_sample.velocities), kVertexScope));
	m_param_rotations.set(OQuatfGeomParam::Sample(QuatfArraySample(path_sample.rotations), kVertexScope));
	m_param_colors.set(OC3fGeomParam::Sample(C3fArraySample(path_sample.colors), kVertexScope));
	m_param_times.set(OFloatGeomParam::Sample(FloatArraySample(path_sample.times), kVertexScope));
}


AbcParticlePathcacheReader::AbcParticlePathcacheReader(const std::string &name, Object *ob, ParticleSystem *psys, ParticleCacheKey ***pathcache, int *totpath, const std::string &suffix) :
    ParticlesReader(ob, psys, name),
    m_pathcache(pathcache),
    m_totpath(totpath),
    m_suffix(suffix)
{
}

void AbcParticlePathcacheReader::init_abc(IObject object)
{
	if (m_curves)
		return;
	m_curves = ICurves(object, kWrapExisting);
	ICurvesSchema &schema = m_curves.getSchema();
	ICompoundProperty geom_props = schema.getArbGeomParams();
	
	m_param_velocities = IV3fGeomParam(geom_props, "velocities", 0);
	m_param_rotations = IQuatfGeomParam(geom_props, "rotations", 0);
	m_param_colors = IV3fGeomParam(geom_props, "colors", 0);
	m_param_times = IFloatGeomParam(geom_props, "times", 0);
}

static void paths_apply_sample_nvertices(ParticleCacheKey **pathcache, int totpart, Int32ArraySamplePtr sample)
{
	int p, k;
	
	BLI_assert(sample->size() == totpart);
	
	const int32_t *data = sample->get();
	
	for (p = 0; p < totpart; ++p) {
		ParticleCacheKey *keys = pathcache[p];
		int num_keys = data[p];
		int segments = num_keys - 1;
		
		for (k = 0; k < num_keys; ++k) {
			keys[k].segments = segments;
		}
	}
}

/* Warning: apply_sample_nvertices has to be called before this! */
static void paths_apply_sample_data(ParticleCacheKey **pathcache, int totpart,
                                    P3fArraySamplePtr sample_pos,
                                    V3fArraySamplePtr sample_vel,
                                    QuatfArraySamplePtr sample_rot,
                                    V3fArraySamplePtr sample_col,
                                    FloatArraySamplePtr sample_time)
{
	int p, k;
	
//	BLI_assert(sample->size() == totvert);
	
	const V3f *data_pos = sample_pos->get();
	const V3f *data_vel = sample_vel->get();
	const Quatf *data_rot = sample_rot->get();
	const V3f *data_col = sample_col->get();
	const float32_t *data_time = sample_time->get();
	ParticleCacheKey **pkeys = pathcache;
	
	for (p = 0; p < totpart; ++p) {
		ParticleCacheKey *key = *pkeys;
		int num_keys = key->segments + 1;
		
		for (k = 0; k < num_keys; ++k) {
			copy_v3_v3(key->co, data_pos->getValue());
			copy_v3_v3(key->vel, data_vel->getValue());
			key->rot[0] = (*data_rot)[0];
			key->rot[1] = (*data_rot)[1];
			key->rot[2] = (*data_rot)[2];
			key->rot[3] = (*data_rot)[3];
			copy_v3_v3(key->col, data_col->getValue());
			key->time = *data_time;
			
			++key;
			++data_pos;
			++data_vel;
			++data_rot;
			++data_col;
			++data_time;
		}
		
		++pkeys;
	}
}

PTCReadSampleResult AbcParticlePathcacheReader::read_sample(float frame)
{
	if (!(*m_pathcache))
		return PTC_READ_SAMPLE_INVALID;
	
	if (!m_curves)
		return PTC_READ_SAMPLE_INVALID;
	
	ISampleSelector ss = abc_archive()->get_frame_sample_selector(frame);
	
	ICurvesSchema &schema = m_curves.getSchema();
	if (schema.getNumSamples() == 0)
		return PTC_READ_SAMPLE_INVALID;
	
	ICurvesSchema::Sample sample;
	schema.get(sample, ss);
	
	P3fArraySamplePtr positions = sample.getPositions();
	Int32ArraySamplePtr nvertices = sample.getCurvesNumVertices();
	IV3fGeomParam::Sample sample_vel = m_param_velocities.getExpandedValue(ss);
	IQuatfGeomParam::Sample sample_rot = m_param_rotations.getExpandedValue(ss);
	IV3fGeomParam::Sample sample_col = m_param_colors.getExpandedValue(ss);
	IFloatGeomParam::Sample sample_time = m_param_times.getExpandedValue(ss);
	
//	int totkeys = positions->size();
	
	if (nvertices->valid()) {
		BLI_assert(nvertices->size() == *m_totpath);
		paths_apply_sample_nvertices(*m_pathcache, *m_totpath, nvertices);
	}
	
	paths_apply_sample_data(*m_pathcache, *m_totpath, positions, sample_vel.getVals(), sample_rot.getVals(), sample_col.getVals(), sample_time.getVals());
	
	return PTC_READ_SAMPLE_EXACT;
}

} /* namespace PTC */