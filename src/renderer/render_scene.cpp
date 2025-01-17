#include "render_scene.h"

#include "engine/array.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/fs/file_system.h"
#include "engine/geometry.h"
#include "engine/json_serializer.h"
#include "engine/lifo_allocator.h"
#include "engine/log.h"
#include "engine/lua_wrapper.h"
#include "engine/math_utils.h"
#include "engine/mtjd/generic_job.h"
#include "engine/mtjd/job.h"
#include "engine/mtjd/manager.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/profiler.h"
#include "engine/property_register.h"
#include "engine/resource_manager.h"
#include "engine/resource_manager_base.h"
#include "engine/serializer.h"
#include "engine/timer.h"

#include "engine/engine.h"

#include "lua_script/lua_script_system.h"

#include "renderer/culling_system.h"
#include "renderer/frame_buffer.h"
#include "renderer/material.h"
#include "renderer/material_manager.h"
#include "renderer/model.h"
#include "renderer/particle_system.h"
#include "renderer/pipeline.h"
#include "renderer/pose.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include "renderer/terrain.h"
#include "renderer/texture.h"
#include "renderer/texture_manager.h"

#include "engine/universe/universe.h"
#include <cmath>
#include <cfloat>


namespace Lumix
{


enum class RenderSceneVersion : int
{
	GRASS_ROTATION_MODE,
	GLOBAL_LIGHT_REFACTOR,

	LATEST
};


static const ComponentType MODEL_INSTANCE_TYPE = PropertyRegister::getComponentType("renderable");
static const ComponentType DECAL_TYPE = PropertyRegister::getComponentType("decal");
static const ComponentType POINT_LIGHT_TYPE = PropertyRegister::getComponentType("point_light");
static const ComponentType PARTICLE_EMITTER_TYPE = PropertyRegister::getComponentType("particle_emitter");
static const ComponentType PARTICLE_EMITTER_ALPHA_TYPE = PropertyRegister::getComponentType("particle_emitter_alpha");
static const ComponentType PARTICLE_EMITTER_FORCE_HASH = PropertyRegister::getComponentType("particle_emitter_force");
static const ComponentType PARTICLE_EMITTER_ATTRACTOR_TYPE =
	PropertyRegister::getComponentType("particle_emitter_attractor");
static const ComponentType PARTICLE_EMITTER_SUBIMAGE_TYPE =
	PropertyRegister::getComponentType("particle_emitter_subimage");
static const ComponentType PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE =
	PropertyRegister::getComponentType("particle_emitter_linear_movement");
static const ComponentType PARTICLE_EMITTER_SPAWN_SHAPE_TYPE =
	PropertyRegister::getComponentType("particle_emitter_spawn_shape");
static const ComponentType PARTICLE_EMITTER_PLANE_TYPE = PropertyRegister::getComponentType("particle_emitter_plane");
static const ComponentType PARTICLE_EMITTER_RANDOM_ROTATION_TYPE =
	PropertyRegister::getComponentType("particle_emitter_random_rotation");
static const ComponentType PARTICLE_EMITTER_SIZE_TYPE = PropertyRegister::getComponentType("particle_emitter_size");
static const ComponentType GLOBAL_LIGHT_TYPE = PropertyRegister::getComponentType("global_light");
static const ComponentType CAMERA_TYPE = PropertyRegister::getComponentType("camera");
static const ComponentType TERRAIN_TYPE = PropertyRegister::getComponentType("terrain");
static const ComponentType BONE_ATTACHMENT_TYPE = PropertyRegister::getComponentType("bone_attachment");
static const ComponentType ENVIRONMENT_PROBE_TYPE = PropertyRegister::getComponentType("environment_probe");

static const ResourceType MATERIAL_TYPE("material");
static const ResourceType TEXTURE_TYPE("texture");
static const ResourceType MODEL_TYPE("model");
static bool is_opengl = false;


struct Decal : public DecalInfo
{
	Entity entity;
	Vec3 scale;
};


struct PointLight
{
	Vec3 m_diffuse_color;
	Vec3 m_specular_color;
	float m_diffuse_intensity;
	float m_specular_intensity;
	Entity m_entity;
	ComponentHandle m_component;
	float m_fov;
	float m_attenuation_param;
	float m_range;
	bool m_cast_shadows;
};


struct GlobalLight
{
	Vec3 m_diffuse_color;
	float m_diffuse_intensity;
	Vec3 m_fog_color;
	float m_fog_density;
	float m_fog_bottom;
	float m_fog_height;
	Entity m_entity;
	Vec4 m_cascades;
};


struct Camera
{
	static const int MAX_SLOT_LENGTH = 30;

	Entity entity;
	float fov;
	float aspect;
	float near;
	float far;
	float ortho_size;
	float screen_width;
	float screen_height;
	bool is_ortho;
	char slot[MAX_SLOT_LENGTH + 1];
};


struct EnvironmentProbe
{
	Texture* texture;
	Texture* irradiance;
	Texture* radiance;
	u64 guid;
};


struct BoneAttachment
{
	Entity entity;
	Entity parent_entity;
	int bone_index;
	Transform relative_transform;
};


class RenderSceneImpl LUMIX_FINAL : public RenderScene
{
private:
	struct ModelLoadedCallback
	{
		ModelLoadedCallback(RenderSceneImpl& scene, Model* model)
			: m_scene(scene)
			, m_ref_count(0)
			, m_model(model)
		{
			m_model->getObserverCb().bind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(&scene);
		}

		~ModelLoadedCallback()
		{
			m_model->getObserverCb().unbind<RenderSceneImpl, &RenderSceneImpl::modelStateChanged>(&m_scene);
		}

		Model* m_model;
		int m_ref_count;
		RenderSceneImpl& m_scene;
	};

public:
	RenderSceneImpl(Renderer& renderer,
		Engine& engine,
		Universe& universe,
		IAllocator& allocator);

	~RenderSceneImpl()
	{
		m_universe.entityTransformed().unbind<RenderSceneImpl, &RenderSceneImpl::onEntityMoved>(this);
		m_universe.entityDestroyed().unbind<RenderSceneImpl, &RenderSceneImpl::onEntityDestroyed>(this);
		CullingSystem::destroy(*m_culling_system);
	}


	void modelStateChanged(Resource::State old_state, Resource::State new_state, Resource& resource)
	{
		Model* model = static_cast<Model*>(&resource);
		if (new_state == Resource::State::READY)
		{
			modelLoaded(model);
		}
		else if (old_state == Resource::State::READY && new_state != Resource::State::READY)
		{
			modelUnloaded(model);
		}
	}


	void clear() override
	{
		auto& rm = m_engine.getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.get(MATERIAL_TYPE));

		m_model_loaded_callbacks.clear();

		for (Decal& decal : m_decals)
		{
			if (decal.material) material_manager->unload(*decal.material);
		}
		m_decals.clear();

		m_cameras.clear();

		for (auto* terrain : m_terrains)
		{
			LUMIX_DELETE(m_allocator, terrain);
		}
		m_terrains.clear();

		for (auto* emitter : m_particle_emitters)
		{
			LUMIX_DELETE(m_allocator, emitter);
		}
		m_particle_emitters.clear();

		for (auto& i : m_model_instances)
		{
			if (i.entity != INVALID_ENTITY && i.model)
			{
				freeCustomMeshes(i, material_manager);
				i.model->getResourceManager().unload(*i.model);
				LUMIX_DELETE(m_allocator, i.pose);
			}
		}
		m_model_instances.clear();
		m_culling_system->clear();

		for (auto& probe : m_environment_probes)
		{
			if (probe.texture) probe.texture->getResourceManager().unload(*probe.texture);
		}
		m_environment_probes.clear();
	}


	void resetParticleEmitter(ComponentHandle cmp) override
	{
		m_particle_emitters[{cmp.index}]->reset();
	}


	ParticleEmitter* getParticleEmitter(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}];
	}


	void updateEmitter(ComponentHandle cmp, float time_delta) override
	{
		m_particle_emitters[{cmp.index}]->update(time_delta);
	}


	Universe& getUniverse() override { return m_universe; }


	ComponentHandle getComponent(Entity entity, ComponentType type) override
	{
		if (type == MODEL_INSTANCE_TYPE)
		{
			if (entity.index >= m_model_instances.size()) return INVALID_COMPONENT;
			ComponentHandle cmp = {entity.index};
			return isValid(m_model_instances[entity.index].entity) ? cmp : INVALID_COMPONENT;
		}
		if (type == ENVIRONMENT_PROBE_TYPE)
		{
			int index = m_environment_probes.find(entity);
			if (index < 0) return INVALID_COMPONENT;
			return {entity.index};
		}
		if (type == DECAL_TYPE)
		{
			int index = m_decals.find(entity);
			if (index < 0) return INVALID_COMPONENT;
			return {entity.index};
		}
		if (type == POINT_LIGHT_TYPE)
		{
			for (auto& i : m_point_lights)
			{
				if (i.m_entity == entity) return i.m_component;
			}
			return INVALID_COMPONENT;
		}
		if (type == GLOBAL_LIGHT_TYPE)
		{
			auto iter = m_global_lights.find(entity);
			if (iter.isValid()) return {entity.index};
			return INVALID_COMPONENT;
		}
		if (type == CAMERA_TYPE)
		{
			auto iter = m_cameras.find(entity);
			ComponentHandle cmp = {entity.index};
			return iter.isValid() ? cmp : INVALID_COMPONENT;
		}
		if (type == TERRAIN_TYPE)
		{
			auto iter = m_terrains.find(entity);
			if (!iter.isValid()) return INVALID_COMPONENT;
			return {entity.index};
		}
		if (type == PARTICLE_EMITTER_TYPE)
		{
			int index = m_particle_emitters.find(entity);
			if (index < 0) return INVALID_COMPONENT;
			if (m_particle_emitters.at(index)->m_is_valid) return {entity.index};
			return INVALID_COMPONENT;
		}
		if (type == BONE_ATTACHMENT_TYPE)
		{
			for (auto& attachment : m_bone_attachments)
			{
				if (attachment.entity == entity)
				{
					return {entity.index};
				}
			}
			return INVALID_COMPONENT;
		}

		static const ComponentType EMITTER_MODULES[] = {ParticleEmitter::AttractorModule::s_type,
			ParticleEmitter::AlphaModule::s_type,
			ParticleEmitter::ForceModule::s_type,
			ParticleEmitter::LinearMovementModule::s_type,
			ParticleEmitter::PlaneModule::s_type,
			ParticleEmitter::RandomRotationModule::s_type,
			ParticleEmitter::SizeModule::s_type,
			ParticleEmitter::SubimageModule::s_type,
			ParticleEmitter::SpawnShapeModule::s_type};

		for (auto& module : EMITTER_MODULES)
		{
			if (module == type)
			{
				int index = m_particle_emitters.find(entity);
				if (index < 0) return INVALID_COMPONENT;
				auto* emitter = m_particle_emitters.at(index);

				if (emitter->getModule(type)) return {entity.index};
				return INVALID_COMPONENT;
			}
		}
		return INVALID_COMPONENT;
	}


	IPlugin& getPlugin() const override { return m_renderer; }


	Int2 getParticleEmitterSpawnCount(ComponentHandle cmp) override
	{
		Int2 ret;
		ret.x = m_particle_emitters[{cmp.index}]->m_spawn_count.from;
		ret.y = m_particle_emitters[{cmp.index}]->m_spawn_count.to;
		return ret;
	}


	void setParticleEmitterSpawnCount(ComponentHandle cmp, const Int2& value) override
	{
		m_particle_emitters[{cmp.index}]->m_spawn_count.from = value.x;
		m_particle_emitters[{cmp.index}]->m_spawn_count.to = Math::maximum(value.x, value.y);
	}



	void getRay(ComponentHandle camera_index,
		float x,
		float y,
		Vec3& origin,
		Vec3& dir) override
	{
		Camera& camera = m_cameras[{camera_index.index}];
		origin = m_universe.getPosition(camera.entity);

		float width = camera.screen_width;
		float height = camera.screen_height;
		if (width <= 0 || height <= 0)
		{
			dir = m_universe.getRotation(camera.entity).rotate(Vec3(0, 0, 1));
			return;
		}

		float nx = 2 * (x / width) - 1;
		float ny = 2 * ((height - y) / height) - 1;

		Matrix projection_matrix = getCameraProjection(camera_index);
		Matrix view_matrix = m_universe.getMatrix(camera.entity);

		if (camera.is_ortho)
		{
			float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
			origin += view_matrix.getXVector() * nx * camera.ortho_size * ratio
				+ view_matrix.getYVector() * ny * camera.ortho_size;
		}

		view_matrix.inverse();
		Matrix inverted = (projection_matrix * view_matrix);
		inverted.inverse();

		Vec4 p0 = inverted * Vec4(nx, ny, -1, 1);
		Vec4 p1 = inverted * Vec4(nx, ny, 1, 1);
		p0 *= 1 / p0.w;
		p1 *= 1 / p1.w;
		dir = p1 - p0;
		dir.normalize();
	}


	Frustum getCameraFrustum(ComponentHandle cmp) const override
	{
		const Camera& camera = m_cameras[{cmp.index}];
		Matrix mtx = m_universe.getMatrix(camera.entity);
		Frustum ret;
		float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
		if (camera.is_ortho)
		{
			ret.computeOrtho(mtx.getTranslation(),
				mtx.getZVector(),
				mtx.getYVector(),
				camera.ortho_size * ratio,
				camera.ortho_size,
				camera.near,
				camera.far);
			return ret;
		}
		ret.computePerspective(mtx.getTranslation(),
			-mtx.getZVector(),
			mtx.getYVector(),
			camera.fov,
			ratio,
			camera.near,
			camera.far);

		return ret;
	}


	int getBoneAttachmentIdx(ComponentHandle cmp) const
	{
		for (int i = 0; i < m_bone_attachments.size(); ++i)
		{
			if (m_bone_attachments[i].entity.index == cmp.index) return i;
		}
		return -1;
	}


	void updateBoneAttachment(const BoneAttachment& bone_attachment)
	{
		if (!isValid(bone_attachment.parent_entity)) return;
		ComponentHandle model_instance = getModelInstanceComponent(bone_attachment.parent_entity);
		if (model_instance == INVALID_COMPONENT) return;
		auto* parent_pose = getPose(model_instance);
		if (!parent_pose) return;

		Transform parent_entity_transform = m_universe.getTransform(bone_attachment.parent_entity);
		int idx = bone_attachment.bone_index;
		if (idx < 0 || idx > parent_pose->count) return;
		Transform bone_transform = {parent_pose->positions[idx], parent_pose->rotations[idx]};
		m_universe.setTransform(
			bone_attachment.entity, parent_entity_transform * bone_transform * bone_attachment.relative_transform);
	}


	Entity getBoneAttachmentParent(ComponentHandle cmp) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		return m_bone_attachments[idx].parent_entity;
	}


	void updateRelativeMatrix(BoneAttachment& attachment)
	{
		if (attachment.parent_entity == INVALID_ENTITY) return;
		if (attachment.bone_index < 0) return;
		ComponentHandle model_instance = getModelInstanceComponent(attachment.parent_entity);
		if (model_instance == INVALID_COMPONENT) return;
		Pose* pose = getPose(model_instance);
		if (!pose) return;
		ASSERT(pose->is_absolute);
		if (attachment.bone_index >= pose->count) return;
		Transform bone_transform = {pose->positions[attachment.bone_index], pose->rotations[attachment.bone_index]};

		Transform inv_parent_transform = m_universe.getTransform(attachment.parent_entity) * bone_transform;
		inv_parent_transform = inv_parent_transform.inverted();
		Transform child_transform = m_universe.getTransform(attachment.entity);
		attachment.relative_transform = inv_parent_transform * child_transform;
	}


	Vec3 getBoneAttachmentPosition(ComponentHandle cmp) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return {0, 0, 0};
		return m_bone_attachments[idx].relative_transform.pos;
	}


	void setBoneAttachmentPosition(ComponentHandle cmp, const Vec3& pos) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return;
		BoneAttachment& attachment = m_bone_attachments[idx];
		attachment.relative_transform.pos = pos;
		m_is_updating_attachments = true;
		updateBoneAttachment(attachment);
		m_is_updating_attachments = false;
	}


	Vec3 getBoneAttachmentRotation(ComponentHandle cmp) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return {0, 0, 0};
		return m_bone_attachments[idx].relative_transform.rot.toEuler();
	}


	void setBoneAttachmentRotation(ComponentHandle cmp, const Vec3& rot) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return;
		BoneAttachment& attachment = m_bone_attachments[idx];
		Vec3 euler = rot;
		euler.x = Math::clamp(euler.x, -Math::PI * 0.5f, Math::PI * 0.5f);
		attachment.relative_transform.rot.fromEuler(euler);
		m_is_updating_attachments = true;
		updateBoneAttachment(attachment);
		m_is_updating_attachments = false;
	}

	int getBoneAttachmentBone(ComponentHandle cmp) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return -1;
		return m_bone_attachments[idx].bone_index;
	}


	void setBoneAttachmentBone(ComponentHandle cmp, int value) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		if (idx < 0) return;
		m_bone_attachments[idx].bone_index = value;
		updateRelativeMatrix(m_bone_attachments[idx]);
	}


	void setBoneAttachmentParent(ComponentHandle cmp, Entity entity) override
	{
		int idx = getBoneAttachmentIdx(cmp);
		m_bone_attachments[idx].parent_entity = entity;
		updateRelativeMatrix(m_bone_attachments[idx]);
	}

	void startGame() override
	{
		m_is_game_running = true;
	}


	void stopGame() override
	{
		m_is_game_running = false;
	}


	void update(float dt, bool paused) override
	{
		PROFILE_FUNCTION();
		if (m_is_game_running)
		{
			m_is_updating_attachments = true;
			for (auto& bone_attachment : m_bone_attachments)
			{
				updateBoneAttachment(bone_attachment);
			}
			m_is_updating_attachments = false;
		}

		m_time += dt;
		for (int i = m_debug_triangles.size() - 1; i >= 0; --i)
		{
			float life = m_debug_triangles[i].life;
			if (life < 0)
			{
				m_debug_triangles.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_triangles[i].life = life;
			}
		}

		for(int i = m_debug_lines.size() - 1; i >= 0; --i)
		{
			float life = m_debug_lines[i].life;
			if(life < 0)
			{
				m_debug_lines.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_lines[i].life = life;
			}
		}


		for (int i = m_debug_points.size() - 1; i >= 0; --i)
		{
			float life = m_debug_points[i].life;
			if (life < 0)
			{
				m_debug_points.eraseFast(i);
			}
			else
			{
				life -= dt;
				m_debug_points[i].life = life;
			}
		}

		if (m_is_game_running && !paused)
		{
			for (auto* emitter : m_particle_emitters)
			{
				if (emitter->m_is_valid) emitter->update(dt);
			}
		}
	}


	void serializeModelInstance(ISerializer& serialize, ComponentHandle cmp)
	{
		ModelInstance& r = m_model_instances[{cmp.index}];
		ASSERT(r.entity != INVALID_ENTITY);

		serialize.write("source", r.model ? r.model->getPath().c_str() : "");
		bool has_changed_materials = r.model && r.model->isReady() && r.meshes != &r.model->getMesh(0);
		serialize.write("custom_materials", has_changed_materials ? r.mesh_count : 0);
		if (has_changed_materials)
		{
			for (int i = 0; i < r.mesh_count; ++i)
			{
				serialize.write("", r.meshes[i].material->getPath().c_str());
			}
		}
	}

	void deserializeModelInstance(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		while (entity.index >= m_model_instances.size())
		{
			auto& r = m_model_instances.emplace();
			r.entity = INVALID_ENTITY;
			r.pose = nullptr;
			r.model = nullptr;
			r.meshes = nullptr;
			r.mesh_count = 0;
		}
		auto& r = m_model_instances[entity.index];
		r.entity = entity;
		r.model = nullptr;
		r.pose = nullptr;
		r.custom_meshes = false;
		r.meshes = nullptr;
		r.mesh_count = 0;

		r.matrix = m_universe.getMatrix(r.entity);

		char path[MAX_PATH_LENGTH];
		serializer.read(path, lengthOf(path));

		ComponentHandle cmp = {r.entity.index};
		if (path[0] != 0)
		{
			auto* model = static_cast<Model*>(m_engine.getResourceManager().get(MODEL_TYPE)->load(Path(path)));
			setModel(cmp, model);
		}

		int material_count;
		serializer.read(&material_count);
		if (material_count > 0)
		{
			allocateCustomMeshes(r, material_count);
			for (int j = 0; j < material_count; ++j)
			{
				char path[MAX_PATH_LENGTH];
				serializer.read(path, lengthOf(path));
				setModelInstanceMaterial(cmp, j, Path(path));
			}
		}

		m_universe.addComponent(r.entity, MODEL_INSTANCE_TYPE, this, cmp);
	}


	void serializeGlobalLight(ISerializer& serializer, ComponentHandle cmp)
	{
		GlobalLight& light = m_global_lights[{cmp.index}];
		serializer.write("cascades", light.m_cascades);
		serializer.write("diffuse_color", light.m_diffuse_color);
		serializer.write("diffuse_intensity", light.m_diffuse_intensity);
		serializer.write("fog_bottom", light.m_fog_bottom);
		serializer.write("fog_color", light.m_fog_color);
		serializer.write("fog_density", light.m_fog_density);
		serializer.write("fog_height", light.m_fog_height);
	}


	void deserializeGlobalLight(IDeserializer& serializer, Entity entity, int scene_version)
	{
		GlobalLight light;
		light.m_entity = entity;
		serializer.read(&light.m_cascades);
		if (scene_version < (int)RenderSceneVersion::GLOBAL_LIGHT_REFACTOR)
		{
			ComponentHandle dummy;
			serializer.read(&dummy);
		}
		serializer.read(&light.m_diffuse_color);
		serializer.read(&light.m_diffuse_intensity);
		serializer.read(&light.m_fog_bottom);
		serializer.read(&light.m_fog_color);
		serializer.read(&light.m_fog_density);
		serializer.read(&light.m_fog_height);
		m_global_lights.insert(entity, light);
		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(light.m_entity, GLOBAL_LIGHT_TYPE, this, cmp);
		m_active_global_light_cmp = cmp;
	}
	
	
	void serializePointLight(ISerializer& serializer, ComponentHandle cmp)
	{
		PointLight& light = m_point_lights[m_point_lights_map[cmp]];
		serializer.write("attenuation", light.m_attenuation_param);
		serializer.write("cast_shadow", light.m_cast_shadows);
		serializer.write("component", light.m_component);
		serializer.write("diffuse_color", light.m_diffuse_color);
		serializer.write("diffuse_intensity", light.m_diffuse_intensity);
		serializer.write("fov", light.m_fov);
		serializer.write("range", light.m_range);
		serializer.write("specular_color", light.m_specular_color);
		serializer.write("specular_intensity", light.m_specular_intensity);
	}


	void deserializePointLight(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		m_light_influenced_geometry.emplace(m_allocator);
		PointLight& light = m_point_lights.emplace();
		light.m_entity = entity;
		serializer.read(&light.m_attenuation_param);
		serializer.read(&light.m_cast_shadows);
		serializer.read(&light.m_component);
		serializer.read(&light.m_diffuse_color);
		serializer.read(&light.m_diffuse_intensity);
		serializer.read(&light.m_fov);
		serializer.read(&light.m_range);
		serializer.read(&light.m_specular_color);
		serializer.read(&light.m_specular_intensity);
		m_point_lights_map.insert(light.m_component, m_point_lights.size() - 1);

		m_universe.addComponent(light.m_entity, POINT_LIGHT_TYPE, this, light.m_component);
	}


	void serializeDecal(ISerializer& serializer, ComponentHandle cmp)
	{
	}


	void deserializeDecal(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ResourceManagerBase* material_manager = m_engine.getResourceManager().get(MATERIAL_TYPE);
		Decal& decal = m_decals.insert(entity);
		char tmp[MAX_PATH_LENGTH];
		decal.entity = entity;
		serializer.read(&decal.scale);
		serializer.read(tmp, lengthOf(tmp));
		decal.material = tmp[0] == '\0' ? nullptr : static_cast<Material*>(material_manager->load(Path(tmp)));
		updateDecalInfo(decal);
		m_universe.addComponent(decal.entity, DECAL_TYPE, this, {decal.entity.index});
	}


	void serializeCamera(ISerializer& serialize, ComponentHandle cmp)
	{
		Camera& camera = m_cameras[{cmp.index}];
		serialize.write("far", camera.far);
		serialize.write("fov", camera.fov);
		serialize.write("is_ortho", camera.is_ortho);
		serialize.write("ortho_size", camera.ortho_size);
		serialize.write("near", camera.near);
		serialize.write("slot", camera.slot);
	}


	void deserializeCamera(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		Camera camera;
		camera.entity = entity;
		serializer.read(&camera.far);
		serializer.read(&camera.fov);
		serializer.read(&camera.is_ortho);
		serializer.read(&camera.ortho_size);
		serializer.read(&camera.near);
		serializer.read(camera.slot, lengthOf(camera.slot));
		m_cameras.insert(camera.entity, camera);
		m_universe.addComponent(camera.entity, CAMERA_TYPE, this, {camera.entity.index});
	}


	void serializeBoneAttachment(ISerializer& serializer, ComponentHandle cmp) 
	{
		int idx = getBoneAttachmentIdx(cmp);
		BoneAttachment& attachment = m_bone_attachments[idx];
		serializer.write("bone_index", attachment.bone_index);
		serializer.write("parent", attachment.parent_entity);
		serializer.write("relative_transform", attachment.relative_transform);
	}


	void deserializeBoneAttachment(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		BoneAttachment& bone_attachment = m_bone_attachments.emplace();
		bone_attachment.entity = entity;
		serializer.read(&bone_attachment.bone_index);
		serializer.read(&bone_attachment.parent_entity);
		serializer.read(&bone_attachment.relative_transform);
		ComponentHandle cmp = {bone_attachment.entity.index};
		m_universe.addComponent(bone_attachment.entity, BONE_ATTACHMENT_TYPE, this, cmp);
	}


	void serializeTerrain(ISerializer& serializer, ComponentHandle cmp)
	{
		Terrain* terrain = m_terrains[{cmp.index}];
		serializer.write("layer_mask", terrain->m_layer_mask);
		serializer.write("scale", terrain->m_scale);
		serializer.write("material", terrain->m_material ? terrain->m_material->getPath().c_str() : "");
		serializer.write("grass_count", terrain->m_grass_types.size());
		for (Terrain::GrassType& type : terrain->m_grass_types)
		{
			serializer.write("density", type.m_density);
			serializer.write("distance", type.m_distance);
			serializer.write("rotation_mode", (int)type.m_rotation_mode);
			serializer.write("model", type.m_grass_model ? type.m_grass_model->getPath().c_str() : "");
		}
	}

	void deserializeTerrain(IDeserializer& serializer, Entity entity, int version)
	{
		Terrain* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, entity, *this, m_allocator);
		m_terrains.insert(entity, terrain);
		terrain->m_entity = entity;
		serializer.read(&terrain->m_layer_mask);
		serializer.read(&terrain->m_scale);
		char tmp[MAX_PATH_LENGTH];
		serializer.read(tmp, lengthOf(tmp));
		auto* material = tmp[0] ? m_engine.getResourceManager().get(MATERIAL_TYPE)->load(Path(tmp)) : nullptr;
		terrain->setMaterial((Material*)material);

		int count;
		serializer.read(&count);
		for(int i = 0; i < count; ++i)
		{
			Terrain::GrassType type(*terrain);
			serializer.read(&type.m_density);
			serializer.read(&type.m_distance);
			if (version >= (int)RenderSceneVersion::GRASS_ROTATION_MODE)
			{
				serializer.read((int*)&type.m_rotation_mode);
			}
			type.m_idx = i;
			serializer.read(tmp, lengthOf(tmp));
			terrain->m_grass_types.push(type);
			terrain->setGrassTypePath(terrain->m_grass_types.size() - 1, Path(tmp));
		}

		m_universe.addComponent(entity, TERRAIN_TYPE, this, { entity.index });
	}

	void serializeEnvironmentProbe(ISerializer& serializer, ComponentHandle cmp) 
	{
		serializer.write("guid", m_environment_probes[{cmp.index}].guid);
	}


	int getVersion() const override { return (int)RenderSceneVersion::LATEST; }


	void deserializeEnvironmentProbe(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		auto* texture_manager = m_engine.getResourceManager().get(TEXTURE_TYPE);
		StaticString<Lumix::MAX_PATH_LENGTH> probe_dir("universes/", m_universe.getName(), "/probes/");
		EnvironmentProbe& probe = m_environment_probes.insert(entity);
		serializer.read(&probe.guid);
		StaticString<Lumix::MAX_PATH_LENGTH> path_str(probe_dir, probe.guid, ".dds");
		probe.texture = static_cast<Texture*>(texture_manager->load(Path(path_str)));
		probe.texture->setFlag(BGFX_TEXTURE_SRGB, true);
		StaticString<Lumix::MAX_PATH_LENGTH> irr_path_str(probe_dir, probe.guid, "_irradiance.dds");
		probe.irradiance = static_cast<Texture*>(texture_manager->load(Path(irr_path_str)));
		probe.irradiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
		StaticString<Lumix::MAX_PATH_LENGTH> r_path_str(probe_dir, probe.guid, "_radiance.dds");
		probe.radiance = static_cast<Texture*>(texture_manager->load(Path(r_path_str)));
		probe.radiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);

		m_universe.addComponent(entity, ENVIRONMENT_PROBE_TYPE, this, {entity.index});
	}


	void serializeParticleEmitter(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		serializer.write("autoemit", emitter->m_autoemit);
		serializer.write("local_space", emitter->m_local_space);
		serializer.write("spawn_period_from", emitter->m_spawn_period.from);
		serializer.write("spawn_period_to", emitter->m_spawn_period.to);
		serializer.write("initial_life_from", emitter->m_initial_life.from);
		serializer.write("initial_life_to", emitter->m_initial_life.to);
		serializer.write("initial_size_from", emitter->m_initial_size.from);
		serializer.write("initial_size_to", emitter->m_initial_size.to);
		serializer.write("spawn_count_from", emitter->m_spawn_count.from);
		serializer.write("spawn_count_to", emitter->m_spawn_count.to);
	}


	void deserializeParticleEmitter(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = LUMIX_NEW(m_allocator, ParticleEmitter)(entity, m_universe, m_allocator);
		emitter->m_entity = entity;
		serializer.read(&emitter->m_autoemit);
		serializer.read(&emitter->m_local_space);
		serializer.read(&emitter->m_spawn_period.from);
		serializer.read(&emitter->m_spawn_period.to);
		serializer.read(&emitter->m_initial_life.from);
		serializer.read(&emitter->m_initial_life.to);
		serializer.read(&emitter->m_initial_size.from);
		serializer.read(&emitter->m_initial_size.to);
		serializer.read(&emitter->m_spawn_count.from);
		serializer.read(&emitter->m_spawn_count.to);
		m_particle_emitters.insert(entity, emitter);
		m_universe.addComponent(entity, PARTICLE_EMITTER_TYPE, this, {entity.index});
	}

	void serializeParticleEmitterAlpha(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::AlphaModule*)emitter->getModule(PARTICLE_EMITTER_ALPHA_TYPE);
		serializer.write("count", module->m_values.size());
		for (Vec2 v : module->m_values)
		{
			serializer.write("", v.x);
			serializer.write("", v.y);
		}
	}
	
	
	void deserializeParticleEmitterAlpha(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::AlphaModule)(*emitter);
		int count;
		serializer.read(&count);
		module->m_values.clear();
		for (int i = 0; i < count; ++i)
		{
			Vec2& v = module->m_values.emplace();
			serializer.read(&v.x);
			serializer.read(&v.y);
		}
		module->sample();
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_ALPHA_TYPE, this, {entity.index});
	}


	void serializeParticleEmitterAttractor(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::AttractorModule*)emitter->getModule(PARTICLE_EMITTER_ATTRACTOR_TYPE);
		serializer.write("force", module->m_force);
		serializer.write("count", module->m_count);
		for (int i = 0; i < module->m_count; ++i)
		{
			serializer.write("", module->m_entities[i]);
		}
	}


	void deserializeParticleEmitterAttractor(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::AttractorModule)(*emitter);
		serializer.read(&module->m_force);
		serializer.read(&module->m_count);
		for (int i = 0; i < module->m_count; ++i)
		{
			serializer.read(&module->m_entities[i]);
		}
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_ATTRACTOR_TYPE, this, {entity.index});
	}

	void serializeParticleEmitterForce(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::ForceModule*)emitter->getModule(PARTICLE_EMITTER_FORCE_HASH);
		serializer.write("acceleration", module->m_acceleration);
	}


	void deserializeParticleEmitterForce(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::ForceModule)(*emitter);
		serializer.read(&module->m_acceleration);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_FORCE_HASH, this, {entity.index});
	}


	void serializeParticleEmitterLinearMovement(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::LinearMovementModule*)emitter->getModule(PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE);
		serializer.write("x_from", module->m_x.from);
		serializer.write("x_to", module->m_x.to);
		serializer.write("y_from", module->m_y.from);
		serializer.write("y_to", module->m_y.to);
		serializer.write("z_from", module->m_z.from);
		serializer.write("z_to", module->m_z.to);
	}


	void deserializeParticleEmitterLinearMovement(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::LinearMovementModule)(*emitter);
		serializer.read(&module->m_x.from);
		serializer.read(&module->m_x.to);
		serializer.read(&module->m_y.from);
		serializer.read(&module->m_y.to);
		serializer.read(&module->m_z.from);
		serializer.read(&module->m_z.to);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE, this, {entity.index});
	}


	void serializeParticleEmitterPlane(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::PlaneModule*)emitter->getModule(PARTICLE_EMITTER_PLANE_TYPE);
		serializer.write("bounce", module->m_bounce);
		serializer.write("entities_count", module->m_count);
		for (int i = 0; i < module->m_count; ++i)
		{
			serializer.write("", module->m_entities[i]);
		}
	}


	void deserializeParticleEmitterPlane(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::PlaneModule)(*emitter);
		serializer.read(&module->m_bounce);
		serializer.read(&module->m_count);
		for (int i = 0; i < module->m_count; ++i)
		{
			serializer.read(&module->m_entities[i]);
		}
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_PLANE_TYPE, this, {entity.index});
	}

	void serializeParticleEmitterSpawnShape(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::SpawnShapeModule*)emitter->getModule(PARTICLE_EMITTER_SPAWN_SHAPE_TYPE);
		serializer.write("shape", (u8)module->m_shape);
		serializer.write("radius", module->m_radius);
	}


	void deserializeParticleEmitterSpawnShape(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::SpawnShapeModule)(*emitter);
		serializer.read((u8*)&module->m_shape);
		serializer.read(&module->m_radius);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SPAWN_SHAPE_TYPE, this, {entity.index});
	}

	void serializeParticleEmitterSize(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::SizeModule*)emitter->getModule(PARTICLE_EMITTER_SIZE_TYPE);
		serializer.write("count", module->m_values.size());
		for (Vec2 v : module->m_values)
		{
			serializer.write("", v.x);
			serializer.write("", v.y);
		}
	}


	void deserializeParticleEmitterSize(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::SizeModule)(*emitter);
		int count;
		serializer.read(&count);
		module->m_values.clear();
		for (int i = 0; i < count; ++i)
		{
			Vec2& v = module->m_values.emplace();
			serializer.read(&v.x);
			serializer.read(&v.y);
		}
		module->sample();
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SIZE_TYPE, this, {entity.index});
	}


	void serializeParticleEmitterRandomRotation(ISerializer& serialize, ComponentHandle cmp) {}


	void deserializeParticleEmitterRandomRotation(IDeserializer& serialize, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::RandomRotationModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_RANDOM_ROTATION_TYPE, this, {entity.index});
	}


	void serializeParticleEmitterSubimage(ISerializer& serializer, ComponentHandle cmp)
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		auto* module = (ParticleEmitter::SubimageModule*)emitter->getModule(PARTICLE_EMITTER_SUBIMAGE_TYPE);
		serializer.write("rows", module->rows);
		serializer.write("cols", module->cols);
	}


	void deserializeParticleEmitterSubimage(IDeserializer& serializer, Entity entity, int /*scene_version*/)
	{
		ParticleEmitter* emitter = m_particle_emitters[entity];
		auto* module = LUMIX_NEW(m_allocator, ParticleEmitter::SubimageModule)(*emitter);
		serializer.read(&module->rows);
		serializer.read(&module->cols);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SUBIMAGE_TYPE, this, {entity.index});
	}


	void serializeBoneAttachments(OutputBlob& serializer)
	{
		serializer.write((i32)m_bone_attachments.size());
		for (auto& attachment : m_bone_attachments)
		{
			serializer.write(attachment.bone_index);
			serializer.write(attachment.entity);
			serializer.write(attachment.parent_entity);
		}
	}

	void serializeCameras(OutputBlob& serializer)
	{
		serializer.write((i32)m_cameras.size());
		for (auto& camera : m_cameras)
		{
			serializer.write(camera.entity);
			serializer.write(camera.far);
			serializer.write(camera.fov);
			serializer.write(camera.is_ortho);
			serializer.write(camera.ortho_size);
			serializer.write(camera.near);
			serializer.writeString(camera.slot);
		}
	}

	void serializeLights(OutputBlob& serializer)
	{
		serializer.write((i32)m_point_lights.size());
		for (int i = 0, c = m_point_lights.size(); i < c; ++i)
		{
			serializer.write(m_point_lights[i]);
		}
		serializer.write(m_point_light_last_cmp);

		serializer.write((i32)m_global_lights.size());
		for (const GlobalLight& light : m_global_lights)
		{
			serializer.write(light);
		}
		serializer.write(m_active_global_light_cmp);
	}

	void serializeModelInstances(OutputBlob& serializer)
	{
		serializer.write((i32)m_model_instances.size());
		for (auto& r : m_model_instances)
		{
			serializer.write(r.entity);
			if(r.entity != INVALID_ENTITY)
			{
				serializer.write(r.model ? r.model->getPath().getHash() : 0);
				bool has_changed_materials = r.model && r.model->isReady() && r.meshes != &r.model->getMesh(0);
				serializer.write(has_changed_materials ? r.mesh_count : 0);
				if (has_changed_materials)
				{
					for (int i = 0; i < r.mesh_count; ++i)
					{
						serializer.writeString(r.meshes[i].material->getPath().c_str());
					}
				}
			}
			
		}
	}

	void serializeTerrains(OutputBlob& serializer)
	{
		serializer.write((i32)m_terrains.size());
		for (auto* terrain : m_terrains)
		{
			terrain->serialize(serializer);
		}
	}


	void deserializeDecals(InputBlob& serializer)
	{
		ResourceManagerBase* material_manager = m_engine.getResourceManager().get(MATERIAL_TYPE);
		int count;
		serializer.read(count);
		m_decals.reserve(count);
		for (int i = 0; i < count; ++i)
		{
			char tmp[MAX_PATH_LENGTH];
			Decal decal;
			serializer.read(decal.entity);
			serializer.read(decal.scale);
			serializer.readString(tmp, lengthOf(tmp));
			decal.material = tmp[0] == '\0' ? nullptr : static_cast<Material*>(material_manager->load(Path(tmp)));
			updateDecalInfo(decal);
			m_decals.insert(decal.entity, decal);
			m_universe.addComponent(decal.entity, DECAL_TYPE, this, {decal.entity.index});
		}
	}


	void serializeDecals(OutputBlob& serializer)
	{
		serializer.write(m_decals.size());
		for (auto& decal : m_decals)
		{
			serializer.write(decal.entity);
			serializer.write(decal.scale);
			serializer.writeString(decal.material ? decal.material->getPath().c_str() : "");
		}
	}


	void serializeEnvironmentProbes(OutputBlob& serializer)
	{
		i32 count = m_environment_probes.size();
		serializer.write(count);
		for (int i = 0; i < count; ++i)
		{
			Entity entity = m_environment_probes.getKey(i);
			serializer.write(entity);
			serializer.write(m_environment_probes.at(i).guid);
		}
	}


	void deserializeEnvironmentProbes(InputBlob& serializer)
	{
		i32 count;
		serializer.read(count);
		m_environment_probes.reserve(count);
		auto* texture_manager = m_engine.getResourceManager().get(TEXTURE_TYPE);
		StaticString<Lumix::MAX_PATH_LENGTH> probe_dir("universes/", m_universe.getName(), "/probes/");
		for (int i = 0; i < count; ++i)
		{
			Entity entity;
			serializer.read(entity);
			EnvironmentProbe& probe = m_environment_probes.insert(entity);
			serializer.read(probe.guid);
			StaticString<Lumix::MAX_PATH_LENGTH> path_str(probe_dir, probe.guid, ".dds");
			probe.texture = static_cast<Texture*>(texture_manager->load(Path(path_str)));
			probe.texture->setFlag(BGFX_TEXTURE_SRGB, true);
			StaticString<Lumix::MAX_PATH_LENGTH> irr_path_str(probe_dir, probe.guid, "_irradiance.dds");
			probe.irradiance = static_cast<Texture*>(texture_manager->load(Path(irr_path_str)));
			probe.irradiance->setFlag(BGFX_TEXTURE_SRGB, true);
			probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
			probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
			StaticString<Lumix::MAX_PATH_LENGTH> r_path_str(probe_dir, probe.guid, "_radiance.dds");
			probe.radiance = static_cast<Texture*>(texture_manager->load(Path(r_path_str)));
			probe.radiance->setFlag(BGFX_TEXTURE_SRGB, true);
			probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
			probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);

			ComponentHandle cmp = {entity.index};
			m_universe.addComponent(entity, ENVIRONMENT_PROBE_TYPE, this, cmp);
		}
	}


	void deserializeBoneAttachments(InputBlob& serializer)
	{
		i32 count;
		serializer.read(count);
		m_bone_attachments.resize(count);
		for (int i = 0; i < count; ++i)
		{
			auto& bone_attachment = m_bone_attachments[i];
			serializer.read(bone_attachment.bone_index);
			serializer.read(bone_attachment.entity);
			serializer.read(bone_attachment.parent_entity);
			updateRelativeMatrix(bone_attachment);
			ComponentHandle cmp = {bone_attachment.entity.index};
			m_universe.addComponent(bone_attachment.entity, BONE_ATTACHMENT_TYPE, this, cmp);
		}
	}


	void deserializeParticleEmitters(InputBlob& serializer)
	{
		int count;
		serializer.read(count);
		m_particle_emitters.reserve(count);
		for(int i = 0; i < count; ++i)
		{
			ParticleEmitter* emitter = LUMIX_NEW(m_allocator, ParticleEmitter)(INVALID_ENTITY, m_universe, m_allocator);
			serializer.read(emitter->m_is_valid);
			if (emitter->m_is_valid)
			{
				emitter->deserialize(serializer, m_engine.getResourceManager());
				ComponentHandle cmp = {emitter->m_entity.index};
				if (emitter->m_is_valid) m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_TYPE, this, cmp);
				for (auto* module : emitter->m_modules)
				{
					if (module->getType() == ParticleEmitter::AlphaModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_ALPHA_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::ForceModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_FORCE_HASH, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::SubimageModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_SUBIMAGE_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::SpawnShapeModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_SPAWN_SHAPE_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::AttractorModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_ATTRACTOR_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::LinearMovementModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::PlaneModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_PLANE_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::RandomRotationModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_RANDOM_ROTATION_TYPE, this, cmp);
					}
					else if (module->getType() == ParticleEmitter::SizeModule::s_type)
					{
						m_universe.addComponent(emitter->m_entity, PARTICLE_EMITTER_SIZE_TYPE, this, cmp);
					}
				}
			}
			if (!emitter->m_is_valid && emitter->m_modules.empty())
			{
				LUMIX_DELETE(m_allocator, emitter);
			}
			else
			{
				m_particle_emitters.insert(emitter->m_entity, emitter);
			}
		}
	}


	void serializeParticleEmitters(OutputBlob& serializer)
	{
		serializer.write(m_particle_emitters.size());
		for (auto* emitter : m_particle_emitters)
		{
			serializer.write(emitter->m_is_valid);
			emitter->serialize(serializer);
		}
	}


	void serialize(OutputBlob& serializer) override
	{
		serializeCameras(serializer);
		serializeModelInstances(serializer);
		serializeLights(serializer);
		serializeTerrains(serializer);
		serializeParticleEmitters(serializer);
		serializeBoneAttachments(serializer);
		serializeEnvironmentProbes(serializer);
		serializeDecals(serializer);
	}


	void deserializeCameras(InputBlob& serializer)
	{
		i32 size;
		serializer.read(size);
		m_cameras.rehash(size);
		for (int i = 0; i < size; ++i)
		{
			Camera camera;
			serializer.read(camera.entity);
			serializer.read(camera.far);
			serializer.read(camera.fov);
			serializer.read(camera.is_ortho);
			serializer.read(camera.ortho_size);
			serializer.read(camera.near);
			serializer.readString(camera.slot, lengthOf(camera.slot));

			m_cameras.insert(camera.entity, camera);
			m_universe.addComponent(camera.entity, CAMERA_TYPE, this, {camera.entity.index});
		}
	}

	void deserializeModelInstances(InputBlob& serializer)
	{
		i32 size = 0;
		serializer.read(size);
		m_model_instances.reserve(size);
		for (int i = 0; i < size; ++i)
		{
			auto& r = m_model_instances.emplace();
			serializer.read(r.entity);
			ASSERT(r.entity.index == i || !isValid(r.entity));
			r.model = nullptr;
			r.pose = nullptr;
			r.custom_meshes = false;
			r.meshes = nullptr;
			r.mesh_count = 0;

			if(r.entity != INVALID_ENTITY)
			{
				r.matrix = m_universe.getMatrix(r.entity);

				u32 path;
				serializer.read(path);

				ComponentHandle cmp = { r.entity.index };
				if (path != 0)
				{
					auto* model = static_cast<Model*>(m_engine.getResourceManager().get(MODEL_TYPE)->load(Path(path)));
					setModel(cmp, model);
				}

				int material_count;
				serializer.read(material_count);
				if (material_count > 0)
				{
					allocateCustomMeshes(r, material_count);
					for (int j = 0; j < material_count; ++j)
					{
						char path[MAX_PATH_LENGTH];
						serializer.readString(path, lengthOf(path));
						setModelInstanceMaterial(cmp, j, Path(path));
					}
				}

				m_universe.addComponent(r.entity, MODEL_INSTANCE_TYPE, this, cmp);
			}
		}
	}

	void deserializeLights(InputBlob& serializer)
	{
		i32 size = 0;
		serializer.read(size);
		m_point_lights.resize(size);
		for (int i = 0; i < size; ++i)
		{
			m_light_influenced_geometry.emplace(m_allocator);
			PointLight& light = m_point_lights[i];
			serializer.read(light);
			m_point_lights_map.insert(light.m_component, i);

			m_universe.addComponent(light.m_entity, POINT_LIGHT_TYPE, this, light.m_component);
		}
		serializer.read(m_point_light_last_cmp);

		serializer.read(size);
		for (int i = 0; i < size; ++i)
		{
			GlobalLight light;
			serializer.read(light);
			m_global_lights.insert(light.m_entity, light);
			m_universe.addComponent(light.m_entity, GLOBAL_LIGHT_TYPE, this, {light.m_entity.index});
		}
		serializer.read(m_active_global_light_cmp);
	}

	void deserializeTerrains(InputBlob& serializer)
	{
		i32 size = 0;
		serializer.read(size);
		for (int i = 0; i < size; ++i)
		{
			auto* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, INVALID_ENTITY, *this, m_allocator);
			terrain->deserialize(serializer, m_universe, *this);
			m_terrains.insert(terrain->getEntity(), terrain);
		}
	}


	void deserialize(InputBlob& serializer) override
	{
		deserializeCameras(serializer);
		deserializeModelInstances(serializer);
		deserializeLights(serializer);
		deserializeTerrains(serializer);
		deserializeParticleEmitters(serializer);
		deserializeBoneAttachments(serializer);
		deserializeEnvironmentProbes(serializer);
		deserializeDecals(serializer);
	}


	void destroyBoneAttachment(ComponentHandle component)
	{
		int idx = getBoneAttachmentIdx(component);
		Entity entity = m_bone_attachments[idx].entity;
		m_bone_attachments.eraseFast(idx);
		m_universe.destroyComponent(entity, BONE_ATTACHMENT_TYPE, this, component);
	}


	void destroyEnvironmentProbe(ComponentHandle component)
	{
		Entity entity = {component.index};
		auto& probe = m_environment_probes[entity];
		if (probe.texture) probe.texture->getResourceManager().unload(*probe.texture);
		m_environment_probes.erase(entity);
		m_universe.destroyComponent(entity, ENVIRONMENT_PROBE_TYPE, this, component);
	}


	void destroyModelInstance(ComponentHandle component)
	{
		m_model_instance_destroyed.invoke(component);
		for (int i = 0; i < m_light_influenced_geometry.size(); ++i)
		{
			Array<ComponentHandle>& influenced_geometry = m_light_influenced_geometry[i];
			for (int j = 0; j < influenced_geometry.size(); ++j)
			{
				if (influenced_geometry[j] == component)
				{
					influenced_geometry.erase(j);
					break;
				}
			}
		}

		setModel(component, nullptr);
		auto& model_instance = m_model_instances[component.index];
		Entity entity = model_instance.entity;
		LUMIX_DELETE(m_allocator, model_instance.pose);
		model_instance.pose = nullptr;
		model_instance.entity = INVALID_ENTITY;
		m_universe.destroyComponent(entity, MODEL_INSTANCE_TYPE, this, component);
	}


	void destroyGlobalLight(ComponentHandle component)
	{
		Entity entity = {component.index};

		m_universe.destroyComponent(entity, GLOBAL_LIGHT_TYPE, this, component);

		if (component == m_active_global_light_cmp)
		{
			m_active_global_light_cmp = INVALID_COMPONENT;
		}
		m_global_lights.erase(entity);
	}


	void destroyDecal(ComponentHandle component)
	{
		Entity entity = {component.index};
		m_decals.erase(entity);
		m_universe.destroyComponent(entity, DECAL_TYPE, this, component);
	}


	void destroyPointLight(ComponentHandle component)
	{
		int index = m_point_lights_map[component];
		Entity entity = m_point_lights[index].m_entity;
		m_point_lights.eraseFast(index);
		m_point_lights_map.erase(component);
		m_light_influenced_geometry.eraseFast(index);
		if (index < m_point_lights.size())
		{
			m_point_lights_map[m_point_lights[index].m_component] = index;
		}
		m_universe.destroyComponent(entity, POINT_LIGHT_TYPE, this, component);
	}


	void destroyCamera(ComponentHandle component)
	{
		Entity entity = {component.index};
		m_cameras.erase(entity);
		m_universe.destroyComponent(entity, CAMERA_TYPE, this, component);
	}


	void destroyTerrain(ComponentHandle component)
	{
		Entity entity = {component.index};
		LUMIX_DELETE(m_allocator, m_terrains[entity]);
		m_terrains.erase(entity);
		m_universe.destroyComponent(entity, TERRAIN_TYPE, this, component);
	}


	void destroyParticleEmitter(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		emitter->reset();
		emitter->m_is_valid = false;
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_TYPE, this, component);
		cleanup(emitter);
	}


	void cleanup(ParticleEmitter* emitter)
	{
		if (emitter->m_is_valid) return;
		if (!emitter->m_modules.empty()) return;

		m_particle_emitters.erase(emitter->m_entity);
		LUMIX_DELETE(m_allocator, emitter);
	}


	void destroyParticleEmitterAlpha(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_ALPHA_TYPE);
		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_ALPHA_TYPE, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterForce(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_FORCE_HASH);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_FORCE_HASH, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterSubimage(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_SUBIMAGE_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		emitter->m_subimage_module = nullptr;
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_SUBIMAGE_TYPE, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterAttractor(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_ATTRACTOR_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_ATTRACTOR_TYPE, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterSize(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_SIZE_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_SIZE_TYPE, this, component);
		cleanup(emitter);

	}


	float getParticleEmitterPlaneBounce(ComponentHandle cmp) override
	{
		auto* emitter = m_particle_emitters[{cmp.index}];
		for(auto* module : emitter->m_modules)
		{
			if(module->getType() == ParticleEmitter::PlaneModule::s_type)
			{
				return static_cast<ParticleEmitter::PlaneModule*>(module)->m_bounce;
			}
		}
		return 0;
	}


	void setParticleEmitterPlaneBounce(ComponentHandle cmp, float value) override
	{
		auto* emitter = m_particle_emitters[{cmp.index}];
		for (auto* module : emitter->m_modules)
		{
			if (module->getType() == ParticleEmitter::PlaneModule::s_type)
			{
				static_cast<ParticleEmitter::PlaneModule*>(module)->m_bounce = value;
				break;
			}
		}
	}


	float getParticleEmitterAttractorForce(ComponentHandle cmp) override
	{
		auto* emitter = m_particle_emitters[{cmp.index}];
		for (auto* module : emitter->m_modules)
		{
			if (module->getType() == ParticleEmitter::AttractorModule::s_type)
			{
				return static_cast<ParticleEmitter::AttractorModule*>(module)->m_force;
			}
		}
		return 0;
	}


	void setParticleEmitterAttractorForce(ComponentHandle cmp, float value) override
	{
		auto* emitter = m_particle_emitters[{cmp.index}];
		for (auto* module : emitter->m_modules)
		{
			if (module->getType() == ParticleEmitter::AttractorModule::s_type)
			{
				static_cast<ParticleEmitter::AttractorModule*>(module)->m_force = value;
				break;
			}
		}
	}


	void destroyParticleEmitterPlane(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_PLANE_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_PLANE_TYPE, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterLinearMovement(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE, this, component);
		cleanup(emitter);
	}


	void destroyParticleEmitterSpawnShape(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_SPAWN_SHAPE_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_SPAWN_SHAPE_TYPE, this, component);
		cleanup(emitter);

	}


	void destroyParticleEmitterRandomRotation(ComponentHandle component)
	{
		auto* emitter = m_particle_emitters[{component.index}];
		auto* module = emitter->getModule(PARTICLE_EMITTER_RANDOM_ROTATION_TYPE);

		ASSERT(module);

		LUMIX_DELETE(m_allocator, module);
		emitter->m_modules.eraseItem(module);
		m_universe.destroyComponent(emitter->m_entity, PARTICLE_EMITTER_RANDOM_ROTATION_TYPE, this, component);
		cleanup(emitter);

	}


	void destroyComponent(ComponentHandle component, ComponentType type) override;


	void setParticleEmitterAlpha(ComponentHandle cmp, const Vec2* values, int count) override
	{
		ASSERT(count > 0);
		ASSERT(values[1].x < 0.001f);
		ASSERT(values[count - 2].x > 0.999f);

		auto* alpha_module = getEmitterModule<ParticleEmitter::AlphaModule>(cmp);
		if (!alpha_module) return;

		alpha_module->m_values.resize(count);
		for (int i = 0; i < count; ++i)
		{
			alpha_module->m_values[i] = values[i];
		}
		alpha_module->sample();
	}


	void setParticleEmitterSubimageRows(ComponentHandle cmp, const int& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SubimageModule>(cmp);
		if (module) module->rows = value;
	}


	void setParticleEmitterSubimageCols(ComponentHandle cmp, const int& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SubimageModule>(cmp);
		if (module) module->cols = value;
	}


	int getParticleEmitterSubimageRows(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SubimageModule>(cmp);
		return module ? module->rows : 1;
	}


	int getParticleEmitterSubimageCols(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SubimageModule>(cmp);
		return module ? module->cols : 1;
	}


	void setParticleEmitterAcceleration(ComponentHandle cmp, const Vec3& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::ForceModule>(cmp);
		if (module) module->m_acceleration = value;
	}


	void setParticleEmitterAutoemit(ComponentHandle cmp, bool autoemit) override
	{
		m_particle_emitters[{cmp.index}]->m_autoemit = autoemit;
	}


	bool getParticleEmitterAutoemit(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}]->m_autoemit;
	}


	void setParticleEmitterLocalSpace(ComponentHandle cmp, bool local_space) override
	{
		m_particle_emitters[{cmp.index}]->m_local_space = local_space;
	}


	bool getParticleEmitterLocalSpace(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}]->m_local_space;
	}


	Vec3 getParticleEmitterAcceleration(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::ForceModule>(cmp);
		return module ? module->m_acceleration : Vec3();
	}


	int getParticleEmitterSizeCount(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SizeModule>(cmp);
		return module ? module->m_values.size() : 0; 
	}


	const Vec2* getParticleEmitterSize(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SizeModule>(cmp);
		return module ? &module->m_values[0] : nullptr;
	}


	void setParticleEmitterSize(ComponentHandle cmp, const Vec2* values, int count) override
	{
		ASSERT(count > 0);
		ASSERT(values[0].x < 0.001f);
		ASSERT(values[count-1].x > 0.999f);

		auto* module = getEmitterModule<ParticleEmitter::SizeModule>(cmp);
		if (!module) return;

		auto size_module = static_cast<ParticleEmitter::SizeModule*>(module);
		size_module->m_values.resize(count);
		for (int i = 0; i < count; ++i)
		{
			size_module->m_values[i] = values[i];
		}
		size_module->sample();
	}


	template <typename T>
	T* getEmitterModule(ComponentHandle cmp) const
	{
		auto& modules = m_particle_emitters[{cmp.index}]->m_modules;
		for (auto* module : modules)
		{
			if (module->getType() == T::s_type)
			{
				return static_cast<T*>(module);
			}
		}
		return nullptr;
	}


	int getParticleEmitterAlphaCount(ComponentHandle cmp) override 
	{
		auto* module = getEmitterModule<ParticleEmitter::AlphaModule>(cmp);
		return module ? module->m_values.size() : 0;
	}


	const Vec2* getParticleEmitterAlpha(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AlphaModule>(cmp);
		return module ? &module->m_values[0] : 0;
	}


	Vec2 getParticleEmitterLinearMovementX(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		return module ? Vec2(module->m_x.from, module->m_x.to) : Vec2(0, 0);
	}


	void setParticleEmitterLinearMovementX(ComponentHandle cmp, const Vec2& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		if (module)
		{
			module->m_x = value;
			module->m_x.check();
		}
	}


	Vec2 getParticleEmitterLinearMovementY(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		return module ? Vec2(module->m_y.from, module->m_y.to) : Vec2(0, 0);
	}


	void setParticleEmitterLinearMovementY(ComponentHandle cmp, const Vec2& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		if (module)
		{
			module->m_y = value;
			module->m_y.check();
		}
	}


	Vec2 getParticleEmitterLinearMovementZ(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		return module ? Vec2(module->m_z.from, module->m_z.to) : Vec2(0, 0);
	}


	void setParticleEmitterLinearMovementZ(ComponentHandle cmp, const Vec2& value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::LinearMovementModule>(cmp);
		if (module)
		{
			module->m_z = value;
			module->m_z.check();
		}
	}


	Vec2 getParticleEmitterInitialLife(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}]->m_initial_life;
	}


	Vec2 getParticleEmitterSpawnPeriod(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}]->m_spawn_period;
	}


	void setParticleEmitterInitialLife(ComponentHandle cmp, const Vec2& value) override
	{
		m_particle_emitters[{cmp.index}]->m_initial_life = value;
		m_particle_emitters[{cmp.index}]->m_initial_life.checkZero();
	}


	void setParticleEmitterInitialSize(ComponentHandle cmp, const Vec2& value) override
	{
		m_particle_emitters[{cmp.index}]->m_initial_size = value;
		m_particle_emitters[{cmp.index}]->m_initial_size.checkZero();
	}


	Vec2 getParticleEmitterInitialSize(ComponentHandle cmp) override
	{
		return m_particle_emitters[{cmp.index}]->m_initial_size;
	}


	void setParticleEmitterSpawnPeriod(ComponentHandle cmp, const Vec2& value) override
	{
		auto* emitter = m_particle_emitters[{cmp.index}];
		emitter->m_spawn_period = value;
		emitter->m_spawn_period.from = Math::maximum(0.01f, emitter->m_spawn_period.from);
		emitter->m_spawn_period.checkZero();
	}


	ComponentHandle createCamera(Entity entity)
	{
		Camera camera;
		camera.is_ortho = false;
		camera.ortho_size = 10;
		camera.entity = entity;
		camera.fov = Math::degreesToRadians(60);
		camera.screen_width = 800;
		camera.screen_height = 600;
		camera.aspect = 800.0f / 600.0f;
		camera.near = 0.1f;
		camera.far = 10000.0f;
		camera.slot[0] = '\0';
		if (!isValid(getCameraInSlot("main"))) copyString(camera.slot, "main");
		m_cameras.insert(entity, camera);
		m_universe.addComponent(entity, CAMERA_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createTerrain(Entity entity)
	{
		Terrain* terrain = LUMIX_NEW(m_allocator, Terrain)(m_renderer, entity, *this, m_allocator);
		m_terrains.insert(entity, terrain);
		m_universe.addComponent(entity, TERRAIN_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createComponent(ComponentType type, Entity entity) override;


	ComponentHandle createParticleEmitterRandomRotation(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::RandomRotationModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_RANDOM_ROTATION_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterPlane(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::PlaneModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_PLANE_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterLinearMovement(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::LinearMovementModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterSpawnShape(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::SpawnShapeModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SPAWN_SHAPE_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterAlpha(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::AlphaModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_ALPHA_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterForce(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::ForceModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_FORCE_HASH, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterSubimage(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::SubimageModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SUBIMAGE_TYPE, this, {entity.index});
		return {entity.index};
	}


	ComponentHandle createParticleEmitterAttractor(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::AttractorModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_ATTRACTOR_TYPE, this, { entity.index });
		return{ entity.index };
	}



	ComponentHandle createParticleEmitterSize(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		auto* emitter = m_particle_emitters.at(index);
		auto module = LUMIX_NEW(m_allocator, ParticleEmitter::SizeModule)(*emitter);
		emitter->addModule(module);
		m_universe.addComponent(entity, PARTICLE_EMITTER_SIZE_TYPE, this, { entity.index });
		return{ entity.index };
	}


	ComponentHandle createParticleEmitter(Entity entity)
	{
		int index = allocateParticleEmitter(entity);
		m_particle_emitters.at(index)->init();

		m_universe.addComponent(entity, PARTICLE_EMITTER_TYPE, this, {entity.index});

		return {entity.index};
	}


	int allocateParticleEmitter(Entity entity)
	{
		int index = m_particle_emitters.find(entity);
		if (index >= 0) return index;
		return m_particle_emitters.insert(entity, LUMIX_NEW(m_allocator, ParticleEmitter)(entity, m_universe, m_allocator));
	}


	ModelInstance* getModelInstances() override
	{
		return &m_model_instances[0];
	}


	ModelInstance* getModelInstance(ComponentHandle cmp) override
	{
		return &m_model_instances[cmp.index];
	}


	ComponentHandle getModelInstanceComponent(Entity entity) override
	{
		ComponentHandle cmp = {entity.index};
		if (cmp.index >= m_model_instances.size()) return INVALID_COMPONENT;
		if (!isValid(m_model_instances[cmp.index].entity)) return INVALID_COMPONENT;
		return cmp;
	}


	Frustum getPointLightFrustum(ComponentHandle cmp) const
	{
		const PointLight& light = m_point_lights[m_point_lights_map[cmp]];
		Frustum frustum;
		frustum.computeOrtho(m_universe.getPosition(light.m_entity),
			Vec3(1, 0, 0),
			Vec3(0, 1, 0),
			light.m_range,
			light.m_range,
			-light.m_range,
			light.m_range);

		return frustum;
	}


	void onEntityDestroyed(Entity entity)
	{
		for (auto& i : m_bone_attachments)
		{
			if (i.parent_entity == entity)
			{
				i.parent_entity = INVALID_ENTITY;
				break;
			}
		}
	}


	void onEntityMoved(Entity entity)
	{
		int index = entity.index;
		ComponentHandle cmp = {index};

		if (index < m_model_instances.size() && isValid(m_model_instances[index].entity) &&
			m_model_instances[index].model && m_model_instances[index].model->isReady())
		{
			ModelInstance& r = m_model_instances[index];
			r.matrix = m_universe.getMatrix(entity);
			if (r.model && r.model->isReady())
			{
				float radius = m_universe.getScale(entity) * r.model->getBoundingRadius();
				Vec3 position = m_universe.getPosition(entity);
				m_culling_system->updateBoundingSphere({position, radius}, cmp);
			}

			float bounding_radius = r.model ? r.model->getBoundingRadius() : 1;
			for (int light_idx = 0, c = m_point_lights.size(); light_idx < c; ++light_idx)
			{
				for (int j = 0, c2 = m_light_influenced_geometry[light_idx].size(); j < c2; ++j)
				{
					if(m_light_influenced_geometry[light_idx][j] == cmp)
					{
						m_light_influenced_geometry[light_idx].eraseFast(j);
						break;
					}
				}

				Vec3 pos = m_universe.getPosition(r.entity);
				Frustum frustum = getPointLightFrustum({light_idx});
				if(frustum.isSphereInside(pos, bounding_radius))
				{
					m_light_influenced_geometry[light_idx].push(cmp);
				}
			}
		}

		int decal_idx = m_decals.find(entity);
		if (decal_idx >= 0)
		{
			updateDecalInfo(m_decals.at(decal_idx));
		}

		for (int i = 0, c = m_point_lights.size(); i < c; ++i)
		{
			if (m_point_lights[i].m_entity == entity)
			{
				detectLightInfluencedGeometry(m_point_lights[i].m_component);
				break;
			}
		}

		bool was_updating = m_is_updating_attachments;
		m_is_updating_attachments = true;
		for (auto& attachment : m_bone_attachments)
		{
			if (attachment.parent_entity == entity)
			{
				updateBoneAttachment(attachment);
			}
		}
		m_is_updating_attachments = was_updating;

		if (m_is_updating_attachments || m_is_game_running) return;
		for (auto& attachment : m_bone_attachments)
		{
			if (attachment.entity == entity)
			{
				updateRelativeMatrix(attachment);
				break;
			}
		}

	}

	Engine& getEngine() const override { return m_engine; }


	Entity getTerrainEntity(ComponentHandle cmp) override
	{
		return {cmp.index};
	}


	Vec2 getTerrainResolution(ComponentHandle cmp) override
	{
		auto* terrain = m_terrains[{cmp.index}];
		return Vec2((float)terrain->getWidth(), (float)terrain->getHeight());
	}


	ComponentHandle getFirstTerrain() override
	{
		if (m_terrains.empty()) return INVALID_COMPONENT;
		auto iter = m_terrains.begin();
		return {iter.value()->getEntity().index};
	}


	ComponentHandle getNextTerrain(ComponentHandle cmp) override
	{
		auto iter = m_terrains.find({cmp.index});
		++iter;
		if (!iter.isValid()) return INVALID_COMPONENT;
		return {iter.value()->getEntity().index};
	}


	ComponentHandle getTerrainComponent(Entity entity) override
	{
		auto iter = m_terrains.find(entity);
		if (!iter.isValid()) return INVALID_COMPONENT;
		return {iter.value()->getEntity().index};
	}


	Vec3 getTerrainNormalAt(ComponentHandle cmp, float x, float z) override
	{
		return m_terrains[{cmp.index}]->getNormal(x, z);
	}


	float getTerrainHeightAt(ComponentHandle cmp, float x, float z) override
	{
		return m_terrains[{cmp.index}]->getHeight(x, z);
	}


	AABB getTerrainAABB(ComponentHandle cmp) override
	{
		return m_terrains[{cmp.index}]->getAABB();
	}


	Vec2 getTerrainSize(ComponentHandle cmp) override
	{
		return m_terrains[{cmp.index}]->getSize();
	}


	void setTerrainMaterialPath(ComponentHandle cmp, const Path& path) override
	{
		if (path.isValid())
		{
			Material* material = static_cast<Material*>(m_engine.getResourceManager().get(MATERIAL_TYPE)->load(path));
			m_terrains[{cmp.index}]->setMaterial(material);
		}
		else
		{
			m_terrains[{cmp.index}]->setMaterial(nullptr);
		}
	}


	Material* getTerrainMaterial(ComponentHandle cmp) override { return m_terrains[{cmp.index}]->getMaterial(); }


	void setDecalScale(ComponentHandle cmp, const Vec3& value) override
	{
		Decal& decal = m_decals[{cmp.index}];
		decal.scale = value;
		updateDecalInfo(decal);
	}


	Vec3 getDecalScale(ComponentHandle cmp) override
	{
		return m_decals[{cmp.index}].scale;
	}


	void getDecals(const Frustum& frustum, Array<DecalInfo>& decals) override
	{
		decals.reserve(m_decals.size());
		for (const Decal& decal : m_decals)
		{
			if (!decal.material || !decal.material->isReady()) continue;
			if (frustum.isSphereInside(decal.position, decal.radius)) decals.push(decal);
		}
	}


	void setDecalMaterialPath(ComponentHandle cmp, const Path& path) override
	{
		ResourceManagerBase* material_manager = m_engine.getResourceManager().get(MATERIAL_TYPE);
		Decal& decal = m_decals[{cmp.index}];
		if (decal.material)
		{
			material_manager->unload(*decal.material);
		}
		if (path.isValid())
		{
			decal.material = static_cast<Material*>(material_manager->load(path));
		}
		else
		{
			decal.material = nullptr;
		}
	}


	Path getDecalMaterialPath(ComponentHandle cmp) override
	{
		Decal& decal = m_decals[{cmp.index}];
		return decal.material ? decal.material->getPath() : Path("");
	}


	Path getTerrainMaterialPath(ComponentHandle cmp) override
	{
		Terrain* terrain = m_terrains[{cmp.index}];
		if (terrain->getMaterial())
		{
			return terrain->getMaterial()->getPath();
		}
		else
		{
			return Path("");
		}
	}


	void setTerrainXZScale(ComponentHandle cmp, float scale) override
	{
		m_terrains[{cmp.index}]->setXZScale(scale);
	}

	float getTerrainXZScale(ComponentHandle cmp) override { return m_terrains[{cmp.index}]->getXZScale(); }


	void setTerrainYScale(ComponentHandle cmp, float scale) override
	{
		m_terrains[{cmp.index}]->setYScale(scale);
	}

	float getTerrainYScale(ComponentHandle cmp) override { return m_terrains[{cmp.index}]->getYScale(); }


	Pose* getPose(ComponentHandle cmp) override { return m_model_instances[cmp.index].pose; }


	Entity getModelInstanceEntity(ComponentHandle cmp) override { return m_model_instances[cmp.index].entity; }


	Model* getModelInstanceModel(ComponentHandle cmp) override { return m_model_instances[cmp.index].model; }


	static u64 getLayerMask(ModelInstance& model_instance)
	{
		Model* model = model_instance.model;
		if (!model->isReady()) return 1;
		u64 layer_mask = 0;
		for(int i = 0; i < model->getMeshCount(); ++i)
		{ 
			layer_mask |= model->getMesh(i).material->getRenderLayerMask();
		}
		return layer_mask;
	}


	void showModelInstance(ComponentHandle cmp) override
	{
		auto& model_instance = m_model_instances[cmp.index];
		if (!model_instance.model || !model_instance.model->isReady()) return;

		Sphere sphere(m_universe.getPosition(model_instance.entity), model_instance.model->getBoundingRadius());
		u64 layer_mask = getLayerMask(model_instance);
		if(!m_culling_system->isAdded(cmp)) m_culling_system->addStatic(cmp, sphere, layer_mask);
	}


	void hideModelInstance(ComponentHandle cmp) override
	{
		m_culling_system->removeStatic(cmp);
	}


	Path getModelInstancePath(ComponentHandle cmp) override
	{
		return m_model_instances[cmp.index].model ? m_model_instances[cmp.index].model->getPath() : Path("");
	}


	int getModelInstanceMaterialsCount(ComponentHandle cmp) override
	{
		return m_model_instances[cmp.index].model ? m_model_instances[cmp.index].mesh_count : 0;
	}


	void setModelInstancePath(ComponentHandle cmp, const Path& path) override
	{
		ModelInstance& r = m_model_instances[cmp.index];

		auto* manager = m_engine.getResourceManager().get(MODEL_TYPE);
		if (path.isValid())
		{
			Model* model = static_cast<Model*>(manager->load(path));
			setModel(cmp, model);
		}
		else
		{
			setModel(cmp, nullptr);
		}
		r.matrix = m_universe.getMatrix(r.entity);
	}


	void forceGrassUpdate(ComponentHandle cmp) override { m_terrains[{cmp.index}]->forceGrassUpdate(); }


	void getTerrainInfos(Array<TerrainInfo>& infos, const Vec3& camera_pos) override
	{
		PROFILE_FUNCTION();
		infos.reserve(m_terrains.size());
		for (auto* terrain : m_terrains)
		{
			terrain->getInfos(infos, camera_pos);
		}
	}


	void getGrassInfos(const Frustum& frustum, Array<GrassInfo>& infos, ComponentHandle camera) override
	{
		PROFILE_FUNCTION();

		if (!m_is_grass_enabled) return;

		for (auto* terrain : m_terrains)
		{
			terrain->getGrassInfos(frustum, infos, camera);
		}
	}


	static int LUA_castCameraRay(lua_State* L)
	{
		auto* scene = LuaWrapper::checkArg<RenderSceneImpl*>(L, 1);
		const char* slot = LuaWrapper::checkArg<const char*>(L, 2);
		float x, y;
		ComponentHandle camera_cmp = scene->getCameraInSlot(slot);
		if (!isValid(camera_cmp)) return 0;
		if (lua_gettop(L) > 3)
		{
			x = LuaWrapper::checkArg<float>(L, 3);
			y = LuaWrapper::checkArg<float>(L, 4);
		}
		else
		{
			x = scene->getCameraScreenWidth(camera_cmp) * 0.5f;
			y = scene->getCameraScreenHeight(camera_cmp) * 0.5f;
		}

		Vec3 origin, dir;
		scene->getRay(camera_cmp, x, y, origin, dir);

		RayCastModelHit hit = scene->castRay(origin, dir, INVALID_COMPONENT);
		LuaWrapper::push(L, hit.m_is_hit);
		LuaWrapper::push(L, hit.m_is_hit ? hit.m_origin + hit.m_dir * hit.m_t : Vec3(0, 0, 0));

		return 2;
	}


	static bgfx::TextureHandle* LUA_getTextureHandle(RenderScene* scene, int resource_idx)
	{
		Resource* res = scene->getEngine().getLuaResource(resource_idx);
		if (!res) return nullptr;
		return &static_cast<Texture*>(res)->handle;
	}

	
	static void LUA_setTexturePixel(Texture* texture, int x, int y, u32 value)
	{
		if (!texture) return;
		if (!texture->isReady()) return;
		if (texture->data.empty()) return;
		if (texture->bytes_per_pixel != 4) return;

		x = Math::clamp(x, 0, texture->width - 1);
		y = Math::clamp(y, 0, texture->height - 1);

		((u32*)&texture->data[0])[x + y * texture->width] = value;
	}


	static void LUA_updateTextureData(Texture* texture, int x, int y, int w, int h)
	{
		if (!texture) return;
		if (!texture->isReady()) return;
		if (texture->data.empty()) return;

		texture->onDataUpdated(x, y, w, h);
	}

	
	static int LUA_getTextureWidth(Texture* texture)
	{
		if (!texture) return 0;
		if (!texture->isReady()) return 0;

		return texture->width;
	}


	static int LUA_getTextureHeight(Texture* texture)
	{
		if (!texture) return 0;
		if (!texture->isReady()) return 0;

		return texture->height;
	}


	static float LUA_getTerrainHeightAt(RenderSceneImpl* render_scene, ComponentHandle cmp, int x, int z)
	{
		return render_scene->m_terrains[{cmp.index}]->getHeight(x, z);
	}


	static void LUA_emitParticle(RenderSceneImpl* render_scene, ComponentHandle emitter)
	{
		int idx = render_scene->m_particle_emitters.find({ emitter.index });
		if (idx < 0) return;
		return render_scene->m_particle_emitters.at(idx)->emit();
	}


	void setTerrainHeightAt(ComponentHandle cmp, int x, int z, float height)
	{
		m_terrains[{cmp.index}]->setHeight(x, z, height);
	}

	static u32 LUA_getTexturePixel(Texture* texture, int x, int y)
	{
		if (!texture) return 0;
		if (!texture->isReady()) return 0;
		if (texture->data.empty()) return 0;
		if (texture->bytes_per_pixel != 4) return 0;
		
		x = Math::clamp(x, 0, texture->width - 1);
		y = Math::clamp(y, 0, texture->height - 1);

		return ((u32*)&texture->data[0])[x + y * texture->width];
	}


	static Pipeline* LUA_createPipeline(Engine* engine, const char* path)
	{
		Renderer& renderer = *static_cast<Renderer*>(engine->getPluginManager().getPlugin("renderer"));
		Pipeline* pipeline = Pipeline::create(renderer, Path(path), renderer.getEngine().getAllocator());
		pipeline->load();
		return pipeline;
	}


	static void LUA_destroyPipeline(Pipeline* pipeline)
	{
		Pipeline::destroy(pipeline);
	}


	static void LUA_setPipelineScene(Pipeline* pipeline, RenderScene* scene)
	{
		pipeline->setScene(scene);
	}


	static RenderScene* LUA_getPipelineScene(Pipeline* pipeline)
	{
		return pipeline->getScene();
	}


	static void LUA_pipelineRender(Pipeline* pipeline, int w, int h)
	{
		pipeline->setViewport(0, 0, w, h);
		pipeline->render();
	}


	static bgfx::TextureHandle* LUA_getRenderBuffer(Pipeline* pipeline,
		const char* framebuffer_name,
		int renderbuffer_idx)
	{
		FrameBuffer::RenderBuffer& rb = pipeline->getFramebuffer(framebuffer_name)->getRenderbuffer(renderbuffer_idx);
		return &rb.m_handle;
	}


	static Texture* LUA_getMaterialTexture(Material* material, int texture_index)
	{
		if (!material) return nullptr;
		return material->getTexture(texture_index);
	}


	static void LUA_setModelInstancePath(IScene* scene, int component, const char* path)
	{
		RenderScene* render_scene = (RenderScene*)scene;
		render_scene->setModelInstancePath({component}, Path(path));
	}


	static int LUA_getModelBoneIndex(Model* model, const char* bone)
	{
		if (!model) return 0;
		return model->getBoneIndex(crc32(bone)).value();
	}


	static unsigned int LUA_compareTGA(RenderSceneImpl* scene, const char* path, const char* path_preimage, int min_diff)
	{
		auto& fs = scene->m_engine.getFileSystem();
		auto file1 = fs.open(fs.getDefaultDevice(), Lumix::Path(path), Lumix::FS::Mode::OPEN_AND_READ);
		auto file2 = fs.open(fs.getDefaultDevice(), Lumix::Path(path_preimage), Lumix::FS::Mode::OPEN_AND_READ);
		if (!file1)
		{
			if (file2) fs.close(*file2);
			Lumix::g_log_error.log("render_test") << "Failed to open " << path;
			return 0xffffFFFF;
		}
		else if (!file2)
		{
			fs.close(*file1);
			Lumix::g_log_error.log("render_test") << "Failed to open " << path_preimage;
			return 0xffffFFFF;
		}
		unsigned int result = Lumix::Texture::compareTGA(scene->m_allocator, file1, file2, min_diff);
		fs.close(*file1);
		fs.close(*file2);
		return result;
	}


	static void LUA_makeScreenshot(RenderSceneImpl* scene, const char* path)
	{
		scene->m_renderer.makeScreenshot(Path(path));
	}


	static void LUA_setModelInstanceMaterial(RenderScene* scene,
		ComponentHandle cmp,
		int index,
		const char* path)
	{
		scene->setModelInstanceMaterial(cmp, index, Path(path));
	}


	bool isGrassEnabled() const override
	{
		return m_is_grass_enabled;
	}


	int getGrassRotationMode(ComponentHandle cmp, int index) override
	{
		return (int)m_terrains[{cmp.index}]->getGrassTypeRotationMode(index);
	}


	void setGrassRotationMode(ComponentHandle cmp, int index, int value) override
	{
		m_terrains[{cmp.index}]->setGrassTypeRotationMode(index, (Terrain::GrassType::RotationMode)value);
	}


	float getGrassDistance(ComponentHandle cmp, int index) override
	{
		return m_terrains[{cmp.index}]->getGrassTypeDistance(index);
	}


	void setGrassDistance(ComponentHandle cmp, int index, float value) override
	{
		m_terrains[{cmp.index}]->setGrassTypeDistance(index, value);
	}


	void enableGrass(bool enabled) override { m_is_grass_enabled = enabled; }


	void setGrassDensity(ComponentHandle cmp, int index, int density) override
	{
		m_terrains[{cmp.index}]->setGrassTypeDensity(index, density);
	}


	int getGrassDensity(ComponentHandle cmp, int index) override
	{
		return m_terrains[{cmp.index}]->getGrassTypeDensity(index);
	}


	void setGrassPath(ComponentHandle cmp, int index, const Path& path) override
	{
		m_terrains[{cmp.index}]->setGrassTypePath(index, path);
	}


	Path getGrassPath(ComponentHandle cmp, int index) override
	{
		return m_terrains[{cmp.index}]->getGrassTypePath(index);
	}


	int getGrassCount(ComponentHandle cmp) override
	{
		return m_terrains[{cmp.index}]->getGrassTypeCount();
	}


	void addGrass(ComponentHandle cmp, int index) override
	{
		m_terrains[{cmp.index}]->addGrassType(index);
	}


	void removeGrass(ComponentHandle cmp, int index) override
	{
		m_terrains[{cmp.index}]->removeGrassType(index);
	}


	ComponentHandle getFirstModelInstance() override
	{
		return getNextModelInstance(INVALID_COMPONENT);
	}


	ComponentHandle getNextModelInstance(ComponentHandle cmp) override
	{
		for(int i = cmp.index + 1; i < m_model_instances.size(); ++i)
		{
			if (m_model_instances[i].entity != INVALID_ENTITY) return {i};
		}
		return INVALID_COMPONENT;
	}


	const CullingSystem::Results* cull(const Frustum& frustum, u64 layer_mask)
	{
		PROFILE_FUNCTION();
		if (m_model_instances.empty()) return nullptr;

		m_culling_system->cullToFrustumAsync(frustum, layer_mask);
		return &m_culling_system->getResult();
	}

	
	void runJobs(Array<MTJD::Job*>& jobs, MTJD::Group& sync_point)
	{
		PROFILE_FUNCTION();
		for (int i = 0; i < jobs.size(); ++i)
		{
			m_engine.getMTJDManager().schedule(jobs[i]);
		}
		if (!jobs.empty())
		{
			sync_point.sync();
		}
	}


	void fillTemporaryInfos(const CullingSystem::Results& results, const Frustum& frustum, const Vec3& lod_ref_point)
	{
		PROFILE_FUNCTION();
		m_jobs.clear();

		while (m_temporary_infos.size() < results.size())
		{
			m_temporary_infos.emplace(m_allocator);
		}
		while (m_temporary_infos.size() > results.size())
		{
			m_temporary_infos.pop();
		}

		for (int subresult_index = 0; subresult_index < results.size(); ++subresult_index)
		{
			Array<ModelInstanceMesh>& subinfos = m_temporary_infos[subresult_index];
			subinfos.clear();
			if (results[subresult_index].empty()) continue;

			MTJD::Job* job = MTJD::makeJob(m_engine.getMTJDManager(),
				[&subinfos, this, &results, subresult_index, &frustum, lod_ref_point]()
				{
					PROFILE_BLOCK("Temporary Info Job");
					PROFILE_INT("ModelInstance count", results[subresult_index].size());
					Vec3 ref_point = lod_ref_point;
					float lod_multiplier = m_lod_multiplier;
					if (frustum.fov > 0)
					{
						float t = frustum.fov / Math::degreesToRadians(60.0f);
						lod_multiplier *= t * t;
					}
					const ComponentHandle* LUMIX_RESTRICT raw_subresults = &results[subresult_index][0];
					ModelInstance* LUMIX_RESTRICT model_instances = &m_model_instances[0];
					for (int i = 0, c = results[subresult_index].size(); i < c; ++i)
					{
						const ModelInstance* LUMIX_RESTRICT model_instance = &model_instances[raw_subresults[i].index];
						float squared_distance = (model_instance->matrix.getTranslation() - ref_point).squaredLength();
						squared_distance *= lod_multiplier;

						const Model* LUMIX_RESTRICT model = model_instance->model;
						LODMeshIndices lod = model->getLODMeshIndices(squared_distance);
						for (int j = lod.from, c = lod.to; j <= c; ++j)
						{
							auto& info = subinfos.emplace();
							info.model_instance = raw_subresults[i];
							info.mesh = &model_instance->meshes[j];
						}
					}
				},
				m_allocator);
			job->addDependency(&m_sync_point);
			m_jobs.push(job);
		}
		runJobs(m_jobs, m_sync_point);
	}


	int getClosestPointLights(const Vec3& reference_pos,
									   ComponentHandle* lights,
									   int max_lights) override
	{

		float dists[16];
		ASSERT(max_lights <= lengthOf(dists));
		ASSERT(max_lights > 0);
		if (m_point_lights.empty()) return 0;

		int light_count = 0;
		for (auto light : m_point_lights)
		{
			Vec3 light_pos = m_universe.getPosition(light.m_entity);
			float dist_squared = (reference_pos - light_pos).squaredLength();

			dists[light_count] = dist_squared;
			lights[light_count] = light.m_component;

			for (int i = light_count; i > 0 && dists[i - 1] > dists[i]; --i)
			{
				float tmp = dists[i];
				dists[i] = dists[i - 1];
				dists[i - 1] = tmp;

				ComponentHandle tmp2 = lights[i];
				lights[i] = lights[i - 1];
				lights[i - 1] = tmp2;
			}
			++light_count;
			if (light_count == max_lights)
			{
				break;
			}
		}

		for (int i = max_lights; i < m_point_lights.size(); ++i)
		{
			PointLight& light = m_point_lights[i];
			Vec3 light_pos = m_universe.getPosition(light.m_entity);
			float dist_squared = (reference_pos - light_pos).squaredLength();

			if (dist_squared < dists[max_lights - 1])
			{
				dists[max_lights - 1] = dist_squared;
				lights[max_lights - 1] = light.m_component;

				for (int i = max_lights - 1; i > 0 && dists[i - 1] > dists[i];
					 --i)
				{
					float tmp = dists[i];
					dists[i] = dists[i - 1];
					dists[i - 1] = tmp;

					ComponentHandle tmp2 = lights[i];
					lights[i] = lights[i - 1];
					lights[i - 1] = tmp2;
				}
			}
		}

		return light_count;
	}


	void getPointLights(const Frustum& frustum, Array<ComponentHandle>& lights) override
	{
		for (int i = 0, ci = m_point_lights.size(); i < ci; ++i)
		{
			PointLight& light = m_point_lights[i];

			if (frustum.isSphereInside(m_universe.getPosition(light.m_entity), light.m_range))
			{
				lights.push(light.m_component);
			}
		}
	}


	Entity getCameraEntity(ComponentHandle camera) const override { return {camera.index}; }


	void setLightCastShadows(ComponentHandle cmp, bool cast_shadows) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_cast_shadows = cast_shadows;
	}


	bool getLightCastShadows(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_cast_shadows;
	}


	void getPointLightInfluencedGeometry(ComponentHandle light_cmp,
		const Frustum& frustum,
		Array<ModelInstanceMesh>& infos) override
	{
		PROFILE_FUNCTION();

		int light_index = m_point_lights_map[light_cmp];
		for (int j = 0, cj = m_light_influenced_geometry[light_index].size(); j < cj; ++j)
		{
			ComponentHandle model_instance_cmp = m_light_influenced_geometry[light_index][j];
			ModelInstance& model_instance = m_model_instances[model_instance_cmp.index];
			const Sphere& sphere = m_culling_system->getSphere(model_instance_cmp);
			if (frustum.isSphereInside(sphere.position, sphere.radius))
			{
				for (int k = 0, kc = model_instance.model->getMeshCount(); k < kc; ++k)
				{
					auto& info = infos.emplace();
					info.mesh = &model_instance.model->getMesh(k);
					info.model_instance = model_instance_cmp;
				}
			}
		}
	}


	void getPointLightInfluencedGeometry(ComponentHandle light_cmp, Array<ModelInstanceMesh>& infos) override
	{
		PROFILE_FUNCTION();

		int light_index = m_point_lights_map[light_cmp];
		auto& geoms = m_light_influenced_geometry[light_index];
		for (int j = 0, cj = geoms.size(); j < cj; ++j)
		{
			const ModelInstance& model_instance = m_model_instances[geoms[j].index];
			for (int k = 0, kc = model_instance.model->getMeshCount(); k < kc; ++k)
			{
				auto& info = infos.emplace();
				info.mesh = &model_instance.model->getMesh(k);
				info.model_instance = geoms[j];
			}
		}
	}


	void getModelInstanceEntities(const Frustum& frustum, Array<Entity>& entities) override
	{
		PROFILE_FUNCTION();

		const CullingSystem::Results* results = cull(frustum, ~0ULL);
		if (!results) return;

		for (auto& subresults : *results)
		{
			for (ComponentHandle model_instance_cmp : subresults)
			{
				entities.push(m_model_instances[model_instance_cmp.index].entity);
			}
		}
	}


	Array<Array<ModelInstanceMesh>>& getModelInstanceInfos(const Frustum& frustum,
		const Vec3& lod_ref_point,
		u64 layer_mask) override
	{
		PROFILE_FUNCTION();

		for(auto& i : m_temporary_infos) i.clear();
		const CullingSystem::Results* results = cull(frustum, layer_mask);
		if (!results) return m_temporary_infos;

		fillTemporaryInfos(*results, frustum, lod_ref_point);
		return m_temporary_infos;
	}


	void setCameraSlot(ComponentHandle cmp, const char* slot) override
	{
		auto& camera = m_cameras[{cmp.index}];
		copyString(camera.slot, lengthOf(camera.slot), slot);
	}


	ComponentHandle getCameraComponent(Entity entity)
	{
		auto iter = m_cameras.find(entity);
		if (!iter.isValid()) return INVALID_COMPONENT;
		return {entity.index};
	}

	const char* getCameraSlot(ComponentHandle camera) override { return m_cameras[{camera.index}].slot; }
	float getCameraFOV(ComponentHandle camera) override { return m_cameras[{camera.index}].fov; }
	void setCameraFOV(ComponentHandle camera, float fov) override { m_cameras[{camera.index}].fov = fov; }
	void setCameraNearPlane(ComponentHandle camera, float near_plane) override { m_cameras[{camera.index}].near = near_plane; }
	float getCameraNearPlane(ComponentHandle camera) override { return m_cameras[{camera.index}].near; }
	void setCameraFarPlane(ComponentHandle camera, float far_plane) override { m_cameras[{camera.index}].far = far_plane; }
	float getCameraFarPlane(ComponentHandle camera) override { return m_cameras[{camera.index}].far; }
	float getCameraScreenWidth(ComponentHandle camera) override { return m_cameras[{camera.index}].screen_width; }
	float getCameraScreenHeight(ComponentHandle camera) override { return m_cameras[{camera.index}].screen_height; }


	void setGlobalLODMultiplier(float multiplier) { m_lod_multiplier = multiplier; }
	float getGlobalLODMultiplier() const { return m_lod_multiplier; }


	Matrix getCameraViewProjection(ComponentHandle cmp) override
	{
		Matrix view = m_universe.getMatrix({cmp.index});
		view.fastInverse();
		return getCameraProjection(cmp) * view;
	}


	Matrix getCameraProjection(ComponentHandle cmp) override
	{
		Camera& camera = m_cameras[{cmp.index}];
		Matrix mtx;
		float ratio = camera.screen_height > 0 ? camera.screen_width / camera.screen_height : 1;
		if (camera.is_ortho)
		{
			mtx.setOrtho(-camera.ortho_size * ratio,
				camera.ortho_size * ratio,
				-camera.ortho_size,
				camera.ortho_size,
				camera.near,
				camera.far,
				is_opengl);
		}
		else
		{
			mtx.setPerspective(camera.fov, ratio, camera.near, camera.far, is_opengl);
		}
		return mtx;
	}


	void setCameraScreenSize(ComponentHandle camera, int w, int h) override
	{
		auto& cam = m_cameras[{camera.index}];
		cam.screen_width = (float)w;
		cam.screen_height = (float)h;
		cam.aspect = w / (float)h;
	}


	Vec2 getCameraScreenSize(ComponentHandle camera) override
	{
		auto& cam = m_cameras[{camera.index}];
		return Vec2(cam.screen_width, cam.screen_height);
	}


	float getCameraOrthoSize(ComponentHandle camera) override { return m_cameras[{camera.index}].ortho_size; }
	void setCameraOrthoSize(ComponentHandle camera, float value) override { m_cameras[{camera.index}].ortho_size = value; }
	bool isCameraOrtho(ComponentHandle camera) override { return m_cameras[{camera.index}].is_ortho; }
	void setCameraOrtho(ComponentHandle camera, bool is_ortho) override { m_cameras[{camera.index}].is_ortho = is_ortho; }


	const Array<DebugTriangle>& getDebugTriangles() const override { return m_debug_triangles; }
	const Array<DebugLine>& getDebugLines() const override { return m_debug_lines; }
	const Array<DebugPoint>& getDebugPoints() const override { return m_debug_points; }


	void addDebugSphere(const Vec3& center,
		float radius,
		u32 color,
		float life) override
	{
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = (Math::PI / 180.0f) * 360.0f / COLS;
		int p2 = COLS >> 1;
		int r2 = ROWS >> 1;
		float prev_ci = 1;
		float prev_si = 0;
		for (int y = -r2; y < r2; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);

			for (int i = -p2; i < p2; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);
				addDebugLine(Vec3(center.x + radius * ci * cy,
								  center.y + radius * sy,
								  center.z + radius * si * cy),
							 Vec3(center.x + radius * ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * si * cy1),
							 color,
							 life);
				addDebugLine(Vec3(center.x + radius * ci * cy,
								  center.y + radius * sy,
								  center.z + radius * si * cy),
							 Vec3(center.x + radius * prev_ci * cy,
								  center.y + radius * sy,
								  center.z + radius * prev_si * cy),
							 color,
							 life);
				addDebugLine(Vec3(center.x + radius * prev_ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * prev_si * cy1),
							 Vec3(center.x + radius * ci * cy1,
								  center.y + radius * sy1,
								  center.z + radius * si * cy1),
							 color,
							 life);
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugHalfSphere(const Matrix& transform, float radius, bool top, u32 color, float life)
	{
		Vec3 center = transform.getTranslation();
		Vec3 x_vec = transform.getXVector();
		Vec3 y_vec = transform.getYVector();
		if (!top) y_vec *= -1;
		Vec3 z_vec = transform.getZVector();
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = Math::degreesToRadians(360.0f) / COLS;
		int p2 = COLS >> 1;
		for (int y = 0; y < ROWS >> 1; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);
			float prev_ci = cos(-STEP);
			float prev_si = sin(-STEP);

			Vec3 y_offset = y_vec * sy;
			Vec3 y_offset1 = y_vec * sy1;

			for (int i = 0; i < COLS; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);

				addDebugLine(
					center + radius * (x_vec * ci * cy + z_vec * si * cy + y_offset),
					center + radius * (x_vec * prev_ci * cy + z_vec * prev_si * cy + y_offset),
					color,
					life);
				addDebugLine(
					center + radius * (x_vec * ci * cy + z_vec * si * cy + y_offset),
					center + radius * (x_vec * ci * cy1 + z_vec * si * cy1 + y_offset1),
					color,
					life);

/*				addDebugLine(Vec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					Vec3(center.x + radius * ci * cy1,
						center.y + radius * sy1,
						center.z + radius * si * cy1),
					color,
					life);

				addDebugLine(Vec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					Vec3(center.x + radius * prev_ci * cy,
						center.y + radius * sy,
						center.z + radius * prev_si * cy),
					color,
					life);
				addDebugLine(Vec3(center.x + radius * prev_ci * cy1,
					center.y + radius * sy1,
					center.z + radius * prev_si * cy1),
					Vec3(center.x + radius * ci * cy1,
						center.y + radius * sy1,
						center.z + radius * si * cy1),
					color,
					life);*/
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugHalfSphere(const Vec3& center, float radius, bool top, u32 color, float life)
	{
		static const int COLS = 36;
		static const int ROWS = COLS >> 1;
		static const float STEP = (Math::PI / 180.0f) * 360.0f / COLS;
		int p2 = COLS >> 1;
		int yfrom = top ? 0 : -(ROWS >> 1);
		int yto = top ? ROWS >> 1 : 0;
		for (int y = yfrom; y < yto; ++y)
		{
			float cy = cos(y * STEP);
			float cy1 = cos((y + 1) * STEP);
			float sy = sin(y * STEP);
			float sy1 = sin((y + 1) * STEP);
			float prev_ci = cos((-p2 - 1) * STEP);
			float prev_si = sin((-p2 - 1) * STEP);

			for (int i = -p2; i < p2; ++i)
			{
				float ci = cos(i * STEP);
				float si = sin(i * STEP);
				addDebugLine(Vec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					Vec3(center.x + radius * ci * cy1,
					center.y + radius * sy1,
					center.z + radius * si * cy1),
					color,
					life);
				addDebugLine(Vec3(center.x + radius * ci * cy,
					center.y + radius * sy,
					center.z + radius * si * cy),
					Vec3(center.x + radius * prev_ci * cy,
					center.y + radius * sy,
					center.z + radius * prev_si * cy),
					color,
					life);
				addDebugLine(Vec3(center.x + radius * prev_ci * cy1,
					center.y + radius * sy1,
					center.z + radius * prev_si * cy1),
					Vec3(center.x + radius * ci * cy1,
					center.y + radius * sy1,
					center.z + radius * si * cy1),
					color,
					life);
				prev_ci = ci;
				prev_si = si;
			}
		}
	}


	void addDebugTriangle(const Vec3& p0,
		const Vec3& p1,
		const Vec3& p2,
		u32 color,
		float life) override
	{
		DebugTriangle& tri = m_debug_triangles.emplace();
		tri.p0 = p0;
		tri.p1 = p1;
		tri.p2 = p2;
		tri.color = ARGBToABGR(color);
		tri.life = life;
	}


	void addDebugCapsule(const Vec3& position,
		float height,
		float radius,
		u32 color,
		float life) override
	{
		addDebugHalfSphere(position + Vec3(0, radius, 0), radius, false, color, life);
		addDebugHalfSphere(position + Vec3(0, radius + height, 0), radius, true, color, life);

		Vec3 z_vec(0, 0, 1.0f);
		Vec3 x_vec(1.0f, 0, 0);
		z_vec.normalize();
		x_vec.normalize();
		Vec3 bottom = position + Vec3(0, radius, 0);
		Vec3 top = bottom + Vec3(0, height, 0);
		for (int i = 1; i <= 32; ++i)
		{
			float a = i / 32.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(bottom + x_vec * x + z_vec * z,
				top + x_vec * x + z_vec * z,
				color,
				life);
		}
	}


	void addDebugCapsule(const Matrix& transform,
		float height,
		float radius,
		u32 color,
		float life) override
	{
		Vec3 x_vec = transform.getXVector();
		Vec3 y_vec = transform.getYVector();
		Vec3 z_vec = transform.getZVector();
		Vec3 position = transform.getTranslation();

		Matrix tmp = transform;
		tmp.setTranslation(transform.getTranslation() + y_vec * radius);
		addDebugHalfSphere(tmp, radius, false, color, life);
		tmp.setTranslation(transform.getTranslation() + y_vec * (radius + height));
		addDebugHalfSphere(tmp, radius, true, color, life);

		Vec3 bottom = position + y_vec * radius;
		Vec3 top = bottom + y_vec * height;
		for (int i = 1; i <= 32; ++i)
		{
			float a = i / 32.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(bottom + x_vec * x + z_vec * z, top + x_vec * x + z_vec * z, color, life);
		}
	}



	void addDebugCylinder(const Vec3& position,
								  const Vec3& up,
								  float radius,
								  u32 color,
								  float life) override
	{
		Vec3 z_vec(-up.y, up.x, 0);
		Vec3 x_vec = crossProduct(up, z_vec);
		float prevx = radius;
		float prevz = 0;
		z_vec.normalize();
		x_vec.normalize();
		Vec3 top = position + up;
		for (int i = 1; i <= 32; ++i)
		{
			float a = i / 32.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(position + x_vec * x + z_vec * z,
						 position + x_vec * prevx + z_vec * prevz,
						 color,
						 life);
			addDebugLine(top + x_vec * x + z_vec * z,
						 top + x_vec * prevx + z_vec * prevz,
						 color,
						 life);
			addDebugLine(position + x_vec * x + z_vec * z,
						 top + x_vec * x + z_vec * z,
						 color,
						 life);
			prevx = x;
			prevz = z;
		}
	}


	void addDebugCube(const Vec3& pos,
		const Vec3& dir,
		const Vec3& up,
		const Vec3& right,
		u32 color,
		float life) override
	{
		addDebugLine(pos + dir + up + right, pos + dir + up - right, color, life);
		addDebugLine(pos - dir + up + right, pos - dir + up - right, color, life);
		addDebugLine(pos + dir + up + right, pos - dir + up + right, color, life);
		addDebugLine(pos + dir + up - right, pos - dir + up - right, color, life);

		addDebugLine(pos + dir - up + right, pos + dir - up - right, color, life);
		addDebugLine(pos - dir - up + right, pos - dir - up - right, color, life);
		addDebugLine(pos + dir - up + right, pos - dir - up + right, color, life);
		addDebugLine(pos + dir - up - right, pos - dir - up - right, color, life);

		addDebugLine(pos + dir + up + right, pos + dir - up + right, color, life);
		addDebugLine(pos + dir + up - right, pos + dir - up - right, color, life);
		addDebugLine(pos - dir + up + right, pos - dir - up + right, color, life);
		addDebugLine(pos - dir + up - right, pos - dir - up - right, color, life);

	}


	void addDebugCubeSolid(const Vec3& min,
		const Vec3& max,
		u32 color,
		float life) override
	{
		Vec3 a = min;
		Vec3 b = min;
		Vec3 c = max;

		b.x = max.x;
		c.z = min.z;
		addDebugTriangle(a, c, b, color, life);
		b.x = min.x;
		b.y = max.y;
		addDebugTriangle(a, b, c, color, life);

		b = max;
		c = max;
		a.z = max.z;
		b.y = min.y;
		addDebugTriangle(a, b, c, color, life);
		b.x = min.x;
		b.y = max.y;
		addDebugTriangle(a, c, b, color, life);

		a = min;
		b = min;
		c = max;

		b.x = max.x;
		c.y = min.y;
		addDebugTriangle(a, c, b, color, life);
		b.x = min.x;
		b.z = max.z;
		addDebugTriangle(a, b, c, color, life);

		b = max;
		c = max;
		a.y = max.y;
		b.z = min.z;
		addDebugTriangle(a, c, b, color, life);
		b.x = min.x;
		b.z = max.z;
		addDebugTriangle(a, b, c, color, life);

		a = min;
		b = min;
		c = max;

		b.y = max.y;
		c.x = min.x;
		addDebugTriangle(a, c, b, color, life);
		b.y = min.y;
		b.z = max.z;
		addDebugTriangle(a, b, c, color, life);

		b = max;
		c = max;
		a.x = max.x;
		b.z = min.z;
		addDebugTriangle(a, b, c, color, life);
		b.y = min.y;
		b.z = max.z;
		addDebugTriangle(a, c, b, color, life);
	}



	void addDebugCube(const Vec3& min,
							  const Vec3& max,
							  u32 color,
							  float life) override
	{
		Vec3 a = min;
		Vec3 b = min;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a.set(b.x, b.y, max.z);
		addDebugLine(a, b, color, life);
		b.set(min.x, a.y, a.z);
		addDebugLine(a, b, color, life);
		a.set(b.x, b.y, min.z);
		addDebugLine(a, b, color, life);

		a = min;
		a.y = max.y;
		b = a;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a.set(b.x, b.y, max.z);
		addDebugLine(a, b, color, life);
		b.set(min.x, a.y, a.z);
		addDebugLine(a, b, color, life);
		a.set(b.x, b.y, min.z);
		addDebugLine(a, b, color, life);

		a = min;
		b = a;
		b.y = max.y;
		addDebugLine(a, b, color, life);
		a.x = max.x;
		b.x = max.x;
		addDebugLine(a, b, color, life);
		a.z = max.z;
		b.z = max.z;
		addDebugLine(a, b, color, life);
		a.x = min.x;
		b.x = min.x;
		addDebugLine(a, b, color, life);
	}


	void addDebugOrthoFrustum(const Frustum& frustum, u32 color, float life)
	{
		Vec3 near_center = frustum.position - frustum.direction * frustum.near_distance;
		Vec3 far_center = frustum.position - frustum.direction * frustum.far_distance;

		float width =
			Math::abs(frustum.ds[(int)Frustum::Planes::LEFT] + frustum.ds[(int)Frustum::Planes::RIGHT]);
		float height =
			Math::abs(frustum.ds[(int)Frustum::Planes::TOP] + frustum.ds[(int)Frustum::Planes::BOTTOM]);

		Vec3 up = frustum.up.normalized() * height * 0.5f;
		Vec3 right = crossProduct(frustum.direction, frustum.up) * width * 0.5f;

		Vec3 points[8];

		points[0] = near_center + up + right;
		points[1] = near_center + up - right;
		points[2] = near_center - up - right;
		points[3] = near_center - up + right;

		points[4] = far_center + up + right;
		points[5] = far_center + up - right;
		points[6] = far_center - up - right;
		points[7] = far_center - up + right;

		addDebugLine(points[0], points[1], color, life);
		addDebugLine(points[1], points[2], color, life);
		addDebugLine(points[2], points[3], color, life);
		addDebugLine(points[3], points[0], color, life);

		addDebugLine(points[4], points[5], color, life);
		addDebugLine(points[5], points[6], color, life);
		addDebugLine(points[6], points[7], color, life);
		addDebugLine(points[7], points[4], color, life);

		addDebugLine(points[0], points[4], color, life);
		addDebugLine(points[1], points[5], color, life);
		addDebugLine(points[2], points[6], color, life);
		addDebugLine(points[3], points[7], color, life);
	}


	void addDebugFrustum(const Frustum& frustum, u32 color, float life) override
	{
		if (frustum.fov < 0)
		{
			addDebugOrthoFrustum(frustum, color, life);
		}
		else
		{
			addDebugFrustum(frustum.position,
				frustum.direction,
				frustum.up,
				frustum.fov,
				frustum.ratio,
				frustum.near_distance,
				frustum.far_distance,
				color,
				life);
		}
	}


	void addDebugFrustum(const Vec3& position,
		const Vec3& direction,
		const Vec3& up,
		float fov,
		float ratio,
		float near_distance,
		float far_distance,
		u32 color,
		float life) override
	{
		Vec3 points[8];
		Vec3 near_center = position + direction * near_distance;
		Vec3 far_center = position + direction * far_distance;
		Vec3 right = crossProduct(direction, up);
		float scale = (float)tan(fov * 0.5f);
		Vec3 up_near = up * near_distance * scale;
		Vec3 right_near = right * (near_distance * scale * ratio);

		points[0] = near_center + up_near + right_near;
		points[1] = near_center + up_near - right_near;
		points[2] = near_center - up_near - right_near;
		points[3] = near_center - up_near + right_near;

		Vec3 up_far = up * far_distance * scale;
		Vec3 right_far = right * (far_distance * scale * ratio);

		points[4] = far_center + up_far + right_far;
		points[5] = far_center + up_far - right_far;
		points[6] = far_center - up_far - right_far;
		points[7] = far_center - up_far + right_far;

		addDebugLine(points[0], points[1], color, life);
		addDebugLine(points[1], points[2], color, life);
		addDebugLine(points[2], points[3], color, life);
		addDebugLine(points[3], points[0], color, life);

		addDebugLine(points[4], points[5], color, life);
		addDebugLine(points[5], points[6], color, life);
		addDebugLine(points[6], points[7], color, life);
		addDebugLine(points[7], points[4], color, life);

		addDebugLine(points[0], points[4], color, life);
		addDebugLine(points[1], points[5], color, life);
		addDebugLine(points[2], points[6], color, life);
		addDebugLine(points[3], points[7], color, life);
	}

	void addDebugCircle(const Vec3& center, const Vec3& up, float radius, u32 color, float life) override
	{
		Vec3 z_vec(-up.y, up.x, 0);
		Vec3 x_vec = crossProduct(up, z_vec);
		float prevx = radius;
		float prevz = 0;
		z_vec.normalize();
		x_vec.normalize();
		for (int i = 1; i <= 64; ++i)
		{
			float a = i / 64.0f * 2 * Math::PI;
			float x = cosf(a) * radius;
			float z = sinf(a) * radius;
			addDebugLine(center + x_vec * x + z_vec * z, center + x_vec * prevx + z_vec * prevz, color, life);
			prevx = x;
			prevz = z;
		}
	}

	void addDebugCross(const Vec3& center, float size, u32 color, float life) override
	{
		addDebugLine(center, Vec3(center.x - size, center.y, center.z), color, life);
		addDebugLine(center, Vec3(center.x + size, center.y, center.z), color, life);
		addDebugLine(center, Vec3(center.x, center.y - size, center.z), color, life);
		addDebugLine(center, Vec3(center.x, center.y + size, center.z), color, life);
		addDebugLine(center, Vec3(center.x, center.y, center.z - size), color, life);
		addDebugLine(center, Vec3(center.x, center.y, center.z + size), color, life);
	}


	void addDebugPoint(const Vec3& pos, u32 color, float life) override
	{
		DebugPoint& point = m_debug_points.emplace();
		point.pos = pos;
		point.color = ARGBToABGR(color);
		point.life = life;
	}


	void addDebugCone(const Vec3& vertex,
		const Vec3& dir,
		const Vec3& axis0,
		const Vec3& axis1,
		u32 color,
		float life) override
	{
		Vec3 base_center = vertex + dir;
		Vec3 prev_p = base_center + axis0;
		for (int i = 1; i <= 32; ++i)
		{
			float angle = i / 32.0f * 2 * Math::PI;
			Vec3 x = cosf(angle) * axis0;
			Vec3 z = sinf(angle) * axis1;
			Vec3 p = base_center + x + z;
			addDebugLine(p, prev_p, color, life);
			addDebugLine(vertex, p, color, life);
			prev_p = p;
		}
	}


	static u32 ARGBToABGR(u32 color)
	{
		return ((color & 0xff) << 16) | (color & 0xff00) | ((color & 0xff0000) >> 16) | (color & 0xff000000);
	}


	void addDebugLine(const Vec3& from, const Vec3& to, u32 color, float life) override
	{
		DebugLine& line = m_debug_lines.emplace();
		line.from = from;
		line.to = to;
		line.color = ARGBToABGR(color);
		line.life = life;
	}


	RayCastModelHit castRayTerrain(ComponentHandle cmp, const Vec3& origin, const Vec3& dir) override
	{
		RayCastModelHit hit;
		hit.m_is_hit = false;
		auto iter = m_terrains.find({cmp.index});
		if (!iter.isValid()) return hit;

		auto* terrain = iter.value();
		hit = terrain->castRay(origin, dir);
		hit.m_component = cmp;
		hit.m_component_type = TERRAIN_TYPE;
		hit.m_entity = terrain->getEntity();
		return hit;
	}


	RayCastModelHit castRay(const Vec3& origin, const Vec3& dir, ComponentHandle ignored_model_instance) override
	{
		PROFILE_FUNCTION();
		RayCastModelHit hit;
		hit.m_is_hit = false;
		float cur_dist = FLT_MAX;
		Universe& universe = getUniverse();
		for (int i = 0; i < m_model_instances.size(); ++i)
		{
			auto& r = m_model_instances[i];
			if (ignored_model_instance.index == i || !r.model) continue;

			const Vec3& pos = r.matrix.getTranslation();
			float radius = r.model->getBoundingRadius();
			float scale = universe.getScale(r.entity);
			float dist = (pos - origin).length();
			if (dist - radius * scale > cur_dist) continue;
			
			Vec3 intersection;
			if (dotProduct(pos - origin, pos - origin) < radius * radius ||
				Math::getRaySphereIntersection(origin, dir, pos, radius * scale, intersection))
			{
				RayCastModelHit new_hit = r.model->castRay(origin, dir, r.matrix);
				if (new_hit.m_is_hit && (!hit.m_is_hit || new_hit.m_t < hit.m_t))
				{
					new_hit.m_component = {i};
					new_hit.m_entity = r.entity;
					new_hit.m_component_type = MODEL_INSTANCE_TYPE;
					hit = new_hit;
					hit.m_is_hit = true;
					cur_dist = dir.length() * hit.m_t;
				}
			}
		}

		for (auto* terrain : m_terrains)
		{
			RayCastModelHit terrain_hit = terrain->castRay(origin, dir);
			if (terrain_hit.m_is_hit && (!hit.m_is_hit || terrain_hit.m_t < hit.m_t))
			{
				terrain_hit.m_component = {terrain->getEntity().index};
				terrain_hit.m_component_type = TERRAIN_TYPE;
				terrain_hit.m_entity = terrain->getEntity();
				hit = terrain_hit;
			}
		}

		return hit;
	}

	
	Vec4 getShadowmapCascades(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_cascades;
	}


	void setShadowmapCascades(ComponentHandle cmp, const Vec4& value) override
	{
		Vec4 valid_value = value;
		valid_value.x = Math::maximum(valid_value.x, 0.02f);
		valid_value.y = Math::maximum(valid_value.x + 0.01f, valid_value.y);
		valid_value.z = Math::maximum(valid_value.y + 0.01f, valid_value.z);
		valid_value.w = Math::maximum(valid_value.z + 0.01f, valid_value.w);

		m_global_lights[{cmp.index}].m_cascades = valid_value;
	}


	void setFogDensity(ComponentHandle cmp, float density) override
	{
		m_global_lights[{cmp.index}].m_fog_density = density;
	}


	void setFogColor(ComponentHandle cmp, const Vec3& color) override
	{
		m_global_lights[{cmp.index}].m_fog_color = color;
	}


	float getFogDensity(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_fog_density;
	}


	float getFogBottom(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_fog_bottom;
	}


	void setFogBottom(ComponentHandle cmp, float bottom) override
	{
		m_global_lights[{cmp.index}].m_fog_bottom = bottom;
	}


	float getFogHeight(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_fog_height;
	}


	void setFogHeight(ComponentHandle cmp, float height) override
	{
		m_global_lights[{cmp.index}].m_fog_height = height;
	}


	Vec3 getFogColor(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_fog_color;
	}


	float getLightAttenuation(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_attenuation_param;
	}


	void setLightAttenuation(ComponentHandle cmp, float attenuation) override
	{
		int index = m_point_lights_map[cmp];
		m_point_lights[index].m_attenuation_param = attenuation;
	}


	float getLightRange(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_range;
	}


	void setLightRange(ComponentHandle cmp, float value) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_range = value;
	}


	void setPointLightIntensity(ComponentHandle cmp, float intensity) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_diffuse_intensity = intensity;
	}


	void setGlobalLightIntensity(ComponentHandle cmp, float intensity) override
	{
		m_global_lights[{cmp.index}].m_diffuse_intensity = intensity;
	}


	void setPointLightColor(ComponentHandle cmp, const Vec3& color) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_diffuse_color = color;
	}


	void setGlobalLightColor(ComponentHandle cmp, const Vec3& color) override
	{
		m_global_lights[{cmp.index}].m_diffuse_color = color;
	}

	
	float getPointLightIntensity(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_diffuse_intensity;
	}


	float getGlobalLightIntensity(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_diffuse_intensity;
	}


	Vec3 getPointLightColor(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_diffuse_color;
	}


	void setPointLightSpecularColor(ComponentHandle cmp, const Vec3& color) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_specular_color = color;
	}


	Vec3 getPointLightSpecularColor(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_specular_color;
	}


	void setPointLightSpecularIntensity(ComponentHandle cmp, float intensity) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_specular_intensity = intensity;
	}


	float getPointLightSpecularIntensity(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_specular_intensity;
	}


	Vec3 getGlobalLightColor(ComponentHandle cmp) override
	{
		return m_global_lights[{cmp.index}].m_diffuse_color;
	}


	void setActiveGlobalLight(ComponentHandle cmp) override
	{
		m_active_global_light_cmp = cmp;
	}


	ComponentHandle getActiveGlobalLight() override
	{
		return m_active_global_light_cmp;
	}


	Entity getPointLightEntity(ComponentHandle cmp) const override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_entity;
	}


	Entity getGlobalLightEntity(ComponentHandle cmp) const override
	{
		return m_global_lights[{cmp.index}].m_entity;
	}


	void reloadEnvironmentProbe(ComponentHandle cmp) override
	{
		Entity entity = {cmp.index};
		auto& probe = m_environment_probes[entity];
		auto* texture_manager = m_engine.getResourceManager().get(TEXTURE_TYPE);
		if (probe.texture) texture_manager->unload(*probe.texture);
		StaticString<Lumix::MAX_PATH_LENGTH> path("universes/", m_universe.getName(), "/probes/", probe.guid, ".dds");
		probe.texture = static_cast<Texture*>(texture_manager->load(Path(path)));
		probe.texture->setFlag(BGFX_TEXTURE_SRGB, true);
		path = "universes/";
		path << m_universe.getName() << "/probes/" << probe.guid << "_irradiance.dds";
		probe.irradiance = static_cast<Texture*>(texture_manager->load(Path(path)));
		probe.irradiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.irradiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		probe.irradiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
		path = "universes/";
		path << m_universe.getName() << "/probes/" << probe.guid << "_radiance.dds";
		probe.radiance = static_cast<Texture*>(texture_manager->load(Path(path)));
		probe.radiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.radiance->setFlag(BGFX_TEXTURE_MIN_ANISOTROPIC, true);
		probe.radiance->setFlag(BGFX_TEXTURE_MAG_ANISOTROPIC, true);
	}


	ComponentHandle getNearestEnvironmentProbe(const Vec3& pos) const override
	{
		float nearest_dist_squared = FLT_MAX;
		Entity nearest = INVALID_ENTITY;
		for (int i = 0, c = m_environment_probes.size(); i < c; ++i)
		{
			Entity probe_entity = m_environment_probes.getKey(i);
			Vec3 probe_pos = m_universe.getPosition(probe_entity);
			float dist_squared = (pos - probe_pos).squaredLength();
			if (dist_squared < nearest_dist_squared)
			{
				nearest = probe_entity;
				nearest_dist_squared = dist_squared;
			}
		}
		if (!isValid(nearest)) return INVALID_COMPONENT;
		return {nearest.index};
	}


	Texture* getEnvironmentProbeTexture(ComponentHandle cmp) const override
	{
		Entity entity = {cmp.index};
		return m_environment_probes[entity].texture;
	}


	Texture* getEnvironmentProbeIrradiance(ComponentHandle cmp) const override
	{
		Entity entity = {cmp.index};
		return m_environment_probes[entity].irradiance;
	}


	Texture* getEnvironmentProbeRadiance(ComponentHandle cmp) const override
	{
		Entity entity = {cmp.index};
		return m_environment_probes[entity].radiance;
	}


	u64 getEnvironmentProbeGUID(ComponentHandle cmp) const override
	{
		Entity entity = { cmp.index };
		return m_environment_probes[entity].guid;
	}


	ComponentHandle getCameraInSlot(const char* slot) override
	{
		for (const auto& camera : m_cameras)
		{
			if (equalStrings(camera.slot, slot))
			{
				return {camera.entity.index};
			}
		}
		return INVALID_COMPONENT;
	}


	float getTime() const override { return m_time; }


	void modelUnloaded(Model*, ComponentHandle component)
	{
		auto& r = m_model_instances[component.index];
		if (!r.custom_meshes)
		{
			r.meshes = nullptr;
			r.mesh_count = 0;
		}
		LUMIX_DELETE(m_allocator, r.pose);
		r.pose = nullptr;

		for (int i = 0; i < m_point_lights.size(); ++i)
		{
			m_light_influenced_geometry[i].eraseItemFast(component);
		}
		m_culling_system->removeStatic(component);
	}


	void freeCustomMeshes(ModelInstance& r, MaterialManager* manager)
	{
		if (!r.custom_meshes) return;
		for (int i = 0; i < r.mesh_count; ++i)
		{
			manager->unload(*r.meshes[i].material);
			r.meshes[i].~Mesh();
		}
		m_allocator.deallocate(r.meshes);
		r.meshes = nullptr;
		r.custom_meshes = false;
		r.mesh_count = 0;
	}


	void modelLoaded(Model* model, ComponentHandle component)
	{
		auto& rm = m_engine.getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.get(MATERIAL_TYPE));

		auto& r = m_model_instances[component.index];
		
		if (model->getMesh(0).material->getLayersCount() > 0) r.type = ModelInstance::MULTILAYER;
		else if (model->getBoneCount() > 0) r.type = ModelInstance::SKINNED;
		else r.type = ModelInstance::RIGID;
		float bounding_radius = r.model->getBoundingRadius();
		float scale = m_universe.getScale(r.entity);
		Sphere sphere(r.matrix.getTranslation(), bounding_radius * scale);
		m_culling_system->addStatic(component, sphere, getLayerMask(r));
		ASSERT(!r.pose);
		if (model->getBoneCount() > 0)
		{
			r.pose = LUMIX_NEW(m_allocator, Pose)(m_allocator);
			r.pose->resize(model->getBoneCount());
			model->getPose(*r.pose);
			int skinned_define_idx = m_renderer.getShaderDefineIdx("SKINNED");
			for (int i = 0; i < model->getMeshCount(); ++i)
			{
				model->getMesh(i).material->setDefine(skinned_define_idx, true);
			}
		}
		r.matrix = m_universe.getMatrix(r.entity);
		ASSERT(!r.meshes || r.custom_meshes);
		if (r.meshes)
		{
			allocateCustomMeshes(r, model->getMeshCount());
			for (int i = 0; i < r.mesh_count; ++i)
			{
				auto& src = model->getMesh(i);
				if (!r.meshes[i].material)
				{
					material_manager->load(*src.material);
					r.meshes[i].material = src.material;
				}
				r.meshes[i].set(
					src.attribute_array_offset, src.attribute_array_size, src.indices_offset, src.indices_count);
			}
		}
		else
		{
			r.meshes = &r.model->getMesh(0);
			r.mesh_count = r.model->getMeshCount();
		}

		for (int i = 0; i < m_point_lights.size(); ++i)
		{
			PointLight& light = m_point_lights[i];
			Vec3 t = r.matrix.getTranslation();
			float radius = r.model->getBoundingRadius();
			if ((t - m_universe.getPosition(light.m_entity)).squaredLength() <
				(radius + light.m_range) * (radius + light.m_range))
			{
				m_light_influenced_geometry[i].push(component);
			}
		}
	}


	void modelUnloaded(Model* model)
	{
		for (int i = 0, c = m_model_instances.size(); i < c; ++i)
		{
			if (m_model_instances[i].entity != INVALID_ENTITY && m_model_instances[i].model == model)
			{
				modelUnloaded(model, {i});
			}
		}
	}


	void modelLoaded(Model* model)
	{
		for (int i = 0, c = m_model_instances.size(); i < c; ++i)
		{
			if (m_model_instances[i].entity != INVALID_ENTITY && m_model_instances[i].model == model)
			{
				modelLoaded(model, {i});
			}
		}

		for (auto& attachment : m_bone_attachments)
		{
			if (isValid(attachment.parent_entity) &&
				isValid(m_model_instances[attachment.parent_entity.index].entity) &&
				m_model_instances[attachment.parent_entity.index].model == model)
			{
				updateRelativeMatrix(attachment);
			}
		}
	}


	ModelLoadedCallback& getModelLoadedCallback(Model* model)
	{
		int idx = m_model_loaded_callbacks.find(model);
		if (idx >= 0) return m_model_loaded_callbacks.at(idx);
		return m_model_loaded_callbacks.emplace(model, *this, model);
	}


	void allocateCustomMeshes(ModelInstance& r, int count)
	{
		if (r.custom_meshes && r.mesh_count == count) return;

		ASSERT(r.model);
		auto& rm = r.model->getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.getOwner().get(MATERIAL_TYPE));

		auto* new_meshes = (Mesh*)m_allocator.allocate(count * sizeof(Mesh));
		if (r.meshes)
		{
			for (int i = 0; i < r.mesh_count; ++i)
			{
				new (NewPlaceholder(), new_meshes + i) Mesh(r.meshes[i]);
			}

			if (r.custom_meshes)
			{
				for (int i = count; i < r.mesh_count; ++i)
				{
					material_manager->unload(*r.meshes[i].material);
				}
				for (int i = 0; i < r.mesh_count; ++i)
				{
					r.meshes[i].~Mesh();
				}
				m_allocator.deallocate(r.meshes);
			}
			else
			{
				for (int i = 0; i < r.mesh_count; ++i)
				{
					material_manager->load(*r.meshes[i].material);
				}
			}
		}

		for (int i = r.mesh_count; i < count; ++i)
		{
			new (NewPlaceholder(), new_meshes + i) Mesh(nullptr, 0, 0, 0, 0, "", m_allocator);
		}
		r.meshes = new_meshes;
		r.mesh_count = count;
		r.custom_meshes = true;
	}


	void setModelInstanceMaterial(ComponentHandle cmp, int index, const Path& path) override
	{
		auto& r = m_model_instances[cmp.index];
		if (r.meshes && r.mesh_count > index && r.meshes[index].material && path == r.meshes[index].material->getPath()) return;

		auto& rm = r.model->getResourceManager();
		auto* material_manager = static_cast<MaterialManager*>(rm.getOwner().get(MATERIAL_TYPE));

		int new_count = Math::maximum(i8(index + 1), r.mesh_count);
		allocateCustomMeshes(r, new_count);
		ASSERT(r.meshes);

		if (r.meshes[index].material) material_manager->unload(*r.meshes[index].material);
		auto* new_material = static_cast<Material*>(material_manager->load(path));
		r.meshes[index].material = new_material;
	}


	Path getModelInstanceMaterial(ComponentHandle cmp, int index) override
	{
		auto& r = m_model_instances[cmp.index];
		if (!r.meshes) return Path("");

		return r.meshes[index].material->getPath();
	}


	void setModel(ComponentHandle component, Model* model)
	{
		auto& model_instance = m_model_instances[component.index];
		ASSERT(isValid(model_instance.entity));
		Model* old_model = model_instance.model;
		bool no_change = model == old_model && old_model;
		if (no_change)
		{
			old_model->getResourceManager().unload(*old_model);
			return;
		}
		if (old_model)
		{
			auto& rm = old_model->getResourceManager();
			auto* material_manager = static_cast<MaterialManager*>(rm.getOwner().get(MATERIAL_TYPE));
			freeCustomMeshes(model_instance, material_manager);
			ModelLoadedCallback& callback = getModelLoadedCallback(old_model);
			--callback.m_ref_count;
			if (callback.m_ref_count == 0)
			{
				m_model_loaded_callbacks.erase(old_model);
			}

			if (old_model->isReady())
			{
				m_culling_system->removeStatic(component);
			}
			old_model->getResourceManager().unload(*old_model);
		}
		model_instance.model = model;
		model_instance.meshes = nullptr;
		model_instance.mesh_count = 0;
		LUMIX_DELETE(m_allocator, model_instance.pose);
		model_instance.pose = nullptr;
		if (model)
		{
			ModelLoadedCallback& callback = getModelLoadedCallback(model);
			++callback.m_ref_count;

			if (model->isReady())
			{
				modelLoaded(model, component);
			}
		}
	}

	IAllocator& getAllocator() override { return m_allocator; }


	void detectLightInfluencedGeometry(ComponentHandle cmp)
	{
		Frustum frustum = getPointLightFrustum(cmp);
		m_culling_system->cullToFrustum(frustum, 0xffffFFFF);
		const CullingSystem::Results& results = m_culling_system->getResult();
		auto& influenced_geometry = m_light_influenced_geometry[m_point_lights_map[cmp]];
		influenced_geometry.clear();
		for (int i = 0; i < results.size(); ++i)
		{
			const CullingSystem::Subresults& subresult = results[i];
			influenced_geometry.reserve(influenced_geometry.size() + subresult.size());
			for (int j = 0, c = subresult.size(); j < c; ++j)
			{
				influenced_geometry.push(subresult[j]);
			}
		}
	}


	int getParticleEmitterAttractorCount(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AttractorModule>(cmp);
		return module ? module->m_count : 0;
	}


	void addParticleEmitterAttractor(ComponentHandle cmp, int index) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AttractorModule>(cmp);
		if (!module) return;

		auto* plane_module = static_cast<ParticleEmitter::AttractorModule*>(module);
		if (plane_module->m_count == lengthOf(plane_module->m_entities)) return;

		if (index < 0)
		{
			plane_module->m_entities[plane_module->m_count] = INVALID_ENTITY;
			++plane_module->m_count;
			return;
		}

		for (int i = plane_module->m_count - 1; i > index; --i)
		{
			plane_module->m_entities[i] = plane_module->m_entities[i - 1];
		}
		plane_module->m_entities[index] = INVALID_ENTITY;
		++plane_module->m_count;
	}


	void removeParticleEmitterAttractor(ComponentHandle cmp, int index) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AttractorModule>(cmp);
		if (!module) return;

		for (int i = index; i < module->m_count - 1; ++i)
		{
			module->m_entities[i] = module->m_entities[i + 1];
		}
		--module->m_count;
	}


	Entity getParticleEmitterAttractorEntity(ComponentHandle cmp, int index) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AttractorModule>(cmp);
		return module ? module->m_entities[index] : INVALID_ENTITY;
	}


	void setParticleEmitterAttractorEntity(ComponentHandle cmp, int index, Entity entity) override
	{
		auto* module = getEmitterModule<ParticleEmitter::AttractorModule>(cmp);
		if(module) module->m_entities[index] = entity;
	}


	float getParticleEmitterShapeRadius(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SpawnShapeModule>(cmp);
		return module ? module->m_radius : 0.0f;
	}


	void setParticleEmitterShapeRadius(ComponentHandle cmp, float value) override
	{
		auto* module = getEmitterModule<ParticleEmitter::SpawnShapeModule>(cmp);
		if (module) module->m_radius = value;
	}


	int getParticleEmitterPlaneCount(ComponentHandle cmp) override
	{
		auto* module = getEmitterModule<ParticleEmitter::PlaneModule>(cmp);
		return module ? module->m_count : 0;
	}


	void addParticleEmitterPlane(ComponentHandle cmp, int index) override
	{
		auto* plane_module = getEmitterModule<ParticleEmitter::PlaneModule>(cmp);
		if (!plane_module) return;

		if (plane_module->m_count == lengthOf(plane_module->m_entities)) return;

		if (index < 0)
		{
			plane_module->m_entities[plane_module->m_count] = INVALID_ENTITY;
			++plane_module->m_count;
			return;
		}

		for (int i = plane_module->m_count - 1; i > index; --i)
		{
			plane_module->m_entities[i] = plane_module->m_entities[i - 1];
		}
		plane_module->m_entities[index] = INVALID_ENTITY;
		++plane_module->m_count;
			
	}


	void removeParticleEmitterPlane(ComponentHandle cmp, int index) override
	{
		auto* plane_module = getEmitterModule<ParticleEmitter::PlaneModule>(cmp);
		if (!plane_module) return;

		for (int i = index; i < plane_module->m_count - 1; ++i)
		{
			plane_module->m_entities[i] = plane_module->m_entities[i + 1];
		}
		--plane_module->m_count;
	}


	Entity getParticleEmitterPlaneEntity(ComponentHandle cmp, int index) override
	{
		auto* module = getEmitterModule<ParticleEmitter::PlaneModule>(cmp);
		return module ? module->m_entities[index] : INVALID_ENTITY;
	}


	void setParticleEmitterPlaneEntity(ComponentHandle cmp, int index, Entity entity) override
	{
		auto* module = getEmitterModule<ParticleEmitter::PlaneModule>(cmp);
		if (module) module->m_entities[index] = entity;
	}


	DelegateList<void(ComponentHandle)>& model_instanceCreated() override
	{
		return m_model_instance_created;
	}


	DelegateList<void(ComponentHandle)>& model_instanceDestroyed() override
	{
		return m_model_instance_destroyed;
	}


	float getLightFOV(ComponentHandle cmp) override
	{
		return m_point_lights[m_point_lights_map[cmp]].m_fov;
	}


	void setLightFOV(ComponentHandle cmp, float fov) override
	{
		m_point_lights[m_point_lights_map[cmp]].m_fov = fov;
	}


	ComponentHandle createGlobalLight(Entity entity)
	{
		GlobalLight light;
		light.m_entity = entity;
		light.m_diffuse_color.set(1, 1, 1);
		light.m_diffuse_intensity = 0;
		light.m_fog_color.set(1, 1, 1);
		light.m_fog_density = 0;
		light.m_cascades.set(3, 8, 100, 300);
		light.m_fog_bottom = 0.0f;
		light.m_fog_height = 10.0f;

		ComponentHandle cmp = {entity.index};
		if (m_global_lights.empty()) m_active_global_light_cmp = cmp;

		m_global_lights.insert(entity, light);
		m_universe.addComponent(entity, GLOBAL_LIGHT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createPointLight(Entity entity)
	{
		PointLight& light = m_point_lights.emplace();
		m_light_influenced_geometry.emplace(m_allocator);
		light.m_entity = entity;
		light.m_diffuse_color.set(1, 1, 1);
		light.m_diffuse_intensity = 1;
		++m_point_light_last_cmp.index;
		light.m_component = m_point_light_last_cmp;
		light.m_fov = Math::degreesToRadians(360);
		light.m_specular_color.set(1, 1, 1);
		light.m_specular_intensity = 1;
		light.m_cast_shadows = false;
		light.m_attenuation_param = 2;
		light.m_range = 10;
		m_point_lights_map.insert(light.m_component, m_point_lights.size() - 1);

		m_universe.addComponent(entity, POINT_LIGHT_TYPE, this, light.m_component);

		detectLightInfluencedGeometry(light.m_component);

		return light.m_component;
	}


	void updateDecalInfo(Decal& decal) const
	{
		decal.position = m_universe.getPosition(decal.entity);
		decal.radius = decal.scale.length();
		decal.mtx = m_universe.getMatrix(decal.entity);
		decal.mtx.setXVector(decal.mtx.getXVector() * decal.scale.x);
		decal.mtx.setYVector(decal.mtx.getYVector() * decal.scale.y);
		decal.mtx.setZVector(decal.mtx.getZVector() * decal.scale.z);
		decal.inv_mtx = decal.mtx;
		decal.inv_mtx.inverse();
	}


	ComponentHandle createDecal(Entity entity)
	{
		Decal& decal = m_decals.insert(entity);
		decal.material = nullptr;
		decal.entity = entity;
		decal.scale.set(1, 1, 1);
		updateDecalInfo(decal);

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, DECAL_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createEnvironmentProbe(Entity entity)
	{
		EnvironmentProbe& probe = m_environment_probes.insert(entity);
		auto* texture_manager = m_engine.getResourceManager().get(TEXTURE_TYPE);
		probe.texture = static_cast<Texture*>(texture_manager->load(Path("pipelines/pbr/default_probe.dds")));
		probe.texture->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.irradiance = static_cast<Texture*>(texture_manager->load(Path("pipelines/pbr/default_probe.dds")));
		probe.irradiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.radiance = static_cast<Texture*>(texture_manager->load(Path("pipelines/pbr/default_probe.dds")));
		probe.radiance->setFlag(BGFX_TEXTURE_SRGB, true);
		probe.guid = Math::randGUID();

		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, ENVIRONMENT_PROBE_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createBoneAttachment(Entity entity)
	{
		BoneAttachment& attachment = m_bone_attachments.emplace();
		attachment.entity = entity;
		attachment.parent_entity = INVALID_ENTITY;
		attachment.bone_index = -1;

		ComponentHandle cmp = { entity.index };
		m_universe.addComponent(entity, BONE_ATTACHMENT_TYPE, this, cmp);
		return cmp;
	}


	ComponentHandle createModelInstance(Entity entity)
	{
		while(entity.index >= m_model_instances.size())
		{
			auto& r = m_model_instances.emplace();
			r.entity = INVALID_ENTITY;
			r.model = nullptr;
			r.pose = nullptr;
		}
		auto& r = m_model_instances[entity.index];
		r.entity = entity;
		r.model = nullptr;
		r.meshes = nullptr;
		r.pose = nullptr;
		r.custom_meshes = false;
		r.mesh_count = 0;
		r.matrix = m_universe.getMatrix(entity);
		ComponentHandle cmp = {entity.index};
		m_universe.addComponent(entity, MODEL_INSTANCE_TYPE, this, cmp);
		m_model_instance_created.invoke(cmp);
		return cmp;
	}


	void setParticleEmitterMaterialPath(ComponentHandle cmp, const Path& path) override
	{
		if (!m_particle_emitters[{cmp.index}]) return;

		auto* manager = m_engine.getResourceManager().get(MATERIAL_TYPE);
		Material* material = static_cast<Material*>(manager->load(path));
		m_particle_emitters[{cmp.index}]->setMaterial(material);
	}


	Path getParticleEmitterMaterialPath(ComponentHandle cmp) override
	{
		ParticleEmitter* emitter = m_particle_emitters[{cmp.index}];
		if (!emitter) return Path("");
		if (!emitter->getMaterial()) return Path("");

		return emitter->getMaterial()->getPath();
	}


	const AssociativeArray<Entity, ParticleEmitter*>& getParticleEmitters() const override
	{
		return m_particle_emitters;
	}

private:
	IAllocator& m_allocator;
	Universe& m_universe;
	Renderer& m_renderer;
	Engine& m_engine;
	CullingSystem* m_culling_system;

	ComponentHandle m_point_light_last_cmp;
	Array<Array<ComponentHandle>> m_light_influenced_geometry;
	ComponentHandle m_active_global_light_cmp;
	HashMap<ComponentHandle, int> m_point_lights_map;

	AssociativeArray<Entity, Decal> m_decals;
	Array<ModelInstance> m_model_instances;
	HashMap<Entity, GlobalLight> m_global_lights;
	Array<PointLight> m_point_lights;
	HashMap<Entity, Camera> m_cameras;
	Array<BoneAttachment> m_bone_attachments;
	AssociativeArray<Entity, EnvironmentProbe> m_environment_probes;
	HashMap<Entity, Terrain*> m_terrains;
	AssociativeArray<Entity, ParticleEmitter*> m_particle_emitters;

	Array<DebugTriangle> m_debug_triangles;
	Array<DebugLine> m_debug_lines;
	Array<DebugPoint> m_debug_points;

	Array<Array<ModelInstanceMesh>> m_temporary_infos;
	MTJD::Group m_sync_point;
	Array<MTJD::Job*> m_jobs;

	float m_time;
	float m_lod_multiplier;
	bool m_is_updating_attachments;
	bool m_is_grass_enabled;
	bool m_is_game_running;

	AssociativeArray<Model*, ModelLoadedCallback> m_model_loaded_callbacks;
	DelegateList<void(ComponentHandle)> m_model_instance_created;
	DelegateList<void(ComponentHandle)> m_model_instance_destroyed;
};



#define COMPONENT_TYPE(type, name) \
	{ \
		type \
		, static_cast<Universe::Serialize>(&RenderSceneImpl::serialize##name) \
		, static_cast<Universe::Deserialize>(&RenderSceneImpl::deserialize##name) \
		, &RenderSceneImpl::create##name \
		, &RenderSceneImpl::destroy##name \
	}

static struct
{
	ComponentType type;
	Universe::Serialize serialize;
	Universe::Deserialize deserialize;
	ComponentHandle(RenderSceneImpl::*creator)(Entity);
	void (RenderSceneImpl::*destroyer)(ComponentHandle);
} COMPONENT_INFOS[] = {
	COMPONENT_TYPE(MODEL_INSTANCE_TYPE, ModelInstance),
	COMPONENT_TYPE(GLOBAL_LIGHT_TYPE, GlobalLight),
	COMPONENT_TYPE(POINT_LIGHT_TYPE, PointLight),
	COMPONENT_TYPE(DECAL_TYPE, Decal),
	COMPONENT_TYPE(CAMERA_TYPE, Camera),
	COMPONENT_TYPE(TERRAIN_TYPE, Terrain),
	COMPONENT_TYPE(BONE_ATTACHMENT_TYPE, BoneAttachment),
	COMPONENT_TYPE(ENVIRONMENT_PROBE_TYPE, EnvironmentProbe),
	COMPONENT_TYPE(PARTICLE_EMITTER_TYPE, ParticleEmitter),
	COMPONENT_TYPE(PARTICLE_EMITTER_ALPHA_TYPE, ParticleEmitterAlpha),
	COMPONENT_TYPE(PARTICLE_EMITTER_ATTRACTOR_TYPE, ParticleEmitterAttractor),
	COMPONENT_TYPE(PARTICLE_EMITTER_FORCE_HASH, ParticleEmitterForce),
	COMPONENT_TYPE(PARTICLE_EMITTER_LINEAR_MOVEMENT_TYPE, ParticleEmitterLinearMovement),
	COMPONENT_TYPE(PARTICLE_EMITTER_PLANE_TYPE, ParticleEmitterPlane),
	COMPONENT_TYPE(PARTICLE_EMITTER_RANDOM_ROTATION_TYPE, ParticleEmitterRandomRotation),
	COMPONENT_TYPE(PARTICLE_EMITTER_SIZE_TYPE, ParticleEmitterSize),
	COMPONENT_TYPE(PARTICLE_EMITTER_SPAWN_SHAPE_TYPE, ParticleEmitterSpawnShape),
	COMPONENT_TYPE(PARTICLE_EMITTER_SUBIMAGE_TYPE, ParticleEmitterSubimage)
};

#undef COMPONENT_TYPE

RenderSceneImpl::RenderSceneImpl(Renderer& renderer,
	Engine& engine,
	Universe& universe,
	IAllocator& allocator)
	: m_engine(engine)
	, m_universe(universe)
	, m_renderer(renderer)
	, m_allocator(allocator)
	, m_model_loaded_callbacks(m_allocator)
	, m_model_instances(m_allocator)
	, m_cameras(m_allocator)
	, m_terrains(m_allocator)
	, m_point_lights(m_allocator)
	, m_light_influenced_geometry(m_allocator)
	, m_global_lights(m_allocator)
	, m_decals(m_allocator)
	, m_debug_triangles(m_allocator)
	, m_debug_lines(m_allocator)
	, m_debug_points(m_allocator)
	, m_temporary_infos(m_allocator)
	, m_sync_point(true, m_allocator)
	, m_jobs(m_allocator)
	, m_active_global_light_cmp(INVALID_COMPONENT)
	, m_point_light_last_cmp(INVALID_COMPONENT)
	, m_model_instance_created(m_allocator)
	, m_model_instance_destroyed(m_allocator)
	, m_is_grass_enabled(true)
	, m_is_game_running(false)
	, m_particle_emitters(m_allocator)
	, m_point_lights_map(m_allocator)
	, m_bone_attachments(m_allocator)
	, m_environment_probes(m_allocator)
	, m_lod_multiplier(1.0f)
	, m_time(0)
	, m_is_updating_attachments(false)
{
	is_opengl = renderer.isOpenGL();
	m_universe.entityTransformed().bind<RenderSceneImpl, &RenderSceneImpl::onEntityMoved>(this);
	m_universe.entityDestroyed().bind<RenderSceneImpl, &RenderSceneImpl::onEntityDestroyed>(this);
	m_culling_system = CullingSystem::create(m_engine.getMTJDManager(), m_allocator);
	m_model_instances.reserve(5000);

	for (auto& i : COMPONENT_INFOS)
	{
		universe.registerComponentType(i.type, this, i.serialize, i.deserialize);
	}
}


ComponentHandle RenderSceneImpl::createComponent(ComponentType type, Entity entity)
{
	for (auto& i : COMPONENT_INFOS)
	{
		if (i.type == type)
		{
			return (this->*i.creator)(entity);
		}
	}

	return INVALID_COMPONENT;
}


void RenderSceneImpl::destroyComponent(ComponentHandle component, ComponentType type)
{
	for (auto& i : COMPONENT_INFOS)
	{
		if (i.type == type)
		{
			(this->*i.destroyer)(component);
			return;
		}
	}
	ASSERT(false);
}


RenderScene* RenderScene::createInstance(Renderer& renderer,
	Engine& engine,
	Universe& universe,
	IAllocator& allocator)
{
	return LUMIX_NEW(allocator, RenderSceneImpl)(renderer, engine, universe, allocator);
}


void RenderScene::destroyInstance(RenderScene* scene)
{
	LUMIX_DELETE(scene->getAllocator(), static_cast<RenderSceneImpl*>(scene));
}


void RenderScene::registerLuaAPI(lua_State* L)
{
	Pipeline::registerLuaAPI(L);
	Model::registerLuaAPI(L);

	#define REGISTER_FUNCTION(F)\
		do { \
		auto f = &LuaWrapper::wrapMethod<RenderSceneImpl, decltype(&RenderSceneImpl::F), &RenderSceneImpl::F>; \
		LuaWrapper::createSystemFunction(L, "Renderer", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(setGlobalLODMultiplier);
	REGISTER_FUNCTION(getGlobalLODMultiplier);
	REGISTER_FUNCTION(getCameraViewProjection);
	REGISTER_FUNCTION(getGlobalLightEntity);
	REGISTER_FUNCTION(getActiveGlobalLight);
	REGISTER_FUNCTION(getCameraInSlot);
	REGISTER_FUNCTION(getCameraEntity);
	REGISTER_FUNCTION(getCameraSlot);
	REGISTER_FUNCTION(getCameraComponent);
	REGISTER_FUNCTION(getModelInstanceComponent);
	REGISTER_FUNCTION(getModelInstanceModel);
	REGISTER_FUNCTION(addDebugCross);
	REGISTER_FUNCTION(addDebugLine);
	REGISTER_FUNCTION(addDebugCircle);
	REGISTER_FUNCTION(addDebugSphere);
	REGISTER_FUNCTION(getTerrainMaterial);
	REGISTER_FUNCTION(getTerrainNormalAt);
	REGISTER_FUNCTION(setTerrainHeightAt);
	REGISTER_FUNCTION(hideModelInstance);
	REGISTER_FUNCTION(showModelInstance);

	#undef REGISTER_FUNCTION

	#define REGISTER_FUNCTION(F)\
		do { \
		auto f = &LuaWrapper::wrap<decltype(&RenderSceneImpl::LUA_##F), &RenderSceneImpl::LUA_##F>; \
		LuaWrapper::createSystemFunction(L, "Renderer", #F, f); \
		} while(false) \

	REGISTER_FUNCTION(createPipeline);
	REGISTER_FUNCTION(destroyPipeline);
	REGISTER_FUNCTION(setPipelineScene);
	REGISTER_FUNCTION(getPipelineScene);
	REGISTER_FUNCTION(pipelineRender);
	REGISTER_FUNCTION(getRenderBuffer);
	REGISTER_FUNCTION(getMaterialTexture);
	REGISTER_FUNCTION(getTextureWidth);
	REGISTER_FUNCTION(getTextureHeight);
	REGISTER_FUNCTION(getTexturePixel);
	REGISTER_FUNCTION(setTexturePixel);
	REGISTER_FUNCTION(getTextureHandle);
	REGISTER_FUNCTION(updateTextureData);
	REGISTER_FUNCTION(setModelInstanceMaterial);
	REGISTER_FUNCTION(setModelInstancePath);
	REGISTER_FUNCTION(getModelBoneIndex);
	REGISTER_FUNCTION(makeScreenshot);
	REGISTER_FUNCTION(compareTGA);
	REGISTER_FUNCTION(getTerrainHeightAt);
	REGISTER_FUNCTION(emitParticle);

	LuaWrapper::createSystemFunction(L, "Renderer", "castCameraRay", &RenderSceneImpl::LUA_castCameraRay);

	#undef REGISTER_FUNCTION
}


} // namespace Lumix