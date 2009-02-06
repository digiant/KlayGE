#include <KlayGE/KlayGE.hpp>
#include <KlayGE/ThrowErr.hpp>
#include <KlayGE/Util.hpp>
#include <KlayGE/Math.hpp>
#include <KlayGE/Font.hpp>
#include <KlayGE/Renderable.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/RenderEngine.hpp>
#include <KlayGE/RenderEffect.hpp>
#include <KlayGE/FrameBuffer.hpp>
#include <KlayGE/SceneManager.hpp>
#include <KlayGE/Context.hpp>
#include <KlayGE/ResLoader.hpp>
#include <KlayGE/RenderSettings.hpp>
#include <KlayGE/RenderableHelper.hpp>
#include <KlayGE/SceneObjectHelper.hpp>
#include <KlayGE/Texture.hpp>
#include <KlayGE/RenderLayout.hpp>

#include <KlayGE/Heightmap/Heightmap.hpp>

#include <KlayGE/RenderFactory.hpp>
#include <KlayGE/InputFactory.hpp>

#include <vector>
#include <sstream>
#include <ctime>
#include <boost/tuple/tuple.hpp>
#pragma warning(push)
#pragma warning(disable: 4127 4512)
#include <boost/random.hpp>
#pragma warning(pop)
#include <boost/bind.hpp>

#include "ParticleSystem.hpp"

using namespace std;
using namespace KlayGE;

namespace
{
	bool use_gs = false;

	template <typename ParticleType>
	class GenParticle
	{
	public:
		GenParticle()
			: random_gen_(boost::lagged_fibonacci607(), boost::uniform_real<float>(-0.05f, 0.05f))
		{
		}

		void operator()(ParticleType& par, float4x4 const & mat)
		{
			par.pos = MathLib::transform_coord(float3(0, 0, 0), mat);
			par.vel = MathLib::transform_normal(float3(random_gen_(), 0.2f + abs(random_gen_()) * 3, random_gen_()), mat);
			par.life = 10;
		}

	private:
		boost::variate_generator<boost::lagged_fibonacci607, boost::uniform_real<float> > random_gen_;
	};

	template <typename ParticleType>
	class UpdateParticle
	{
	public:
		UpdateParticle(boost::shared_ptr<HeightImg> hm, std::vector<float3> const & normals)
			: hm_(hm), normals_(normals)
		{
		}

		void operator()(ParticleType& par, float elapse_time)
		{
			par.vel += float3(0, -0.1f, 0) * elapse_time;
			par.pos += par.vel * elapse_time;
			par.life -= elapse_time;

			if (par.pos.y() < (*hm_)(par.pos.x(), par.pos.z()))
			{
				par.pos.y() += 0.01f;
				par.vel = MathLib::reflect(par.vel,
					normals_[hm_->GetImgY(par.pos.z()) * hm_->HMWidth() + hm_->GetImgX(par.pos.x())]) * 0.8f;
			}
		}

	private:
		boost::shared_ptr<HeightImg> hm_;
		std::vector<float3> normals_;
	};

	class TerrainRenderable : public RenderableHelper
	{
	public:
		TerrainRenderable(std::vector<float3> const & vertices, std::vector<uint16_t> const & indices)
			: RenderableHelper(L"Terrain")
		{
			TextureLoader grass = LoadTexture("grass.dds", EAH_GPU_Read);

			RenderFactory& rf = Context::Instance().RenderFactoryInstance();

			technique_ = rf.LoadEffect("ParticleSystem.kfx")->TechniqueByName("Terrain");

			rl_ = rf.MakeRenderLayout();
			rl_->TopologyType(RenderLayout::TT_TriangleList);

			ElementInitData init_data;
			init_data.row_pitch = static_cast<uint32_t>(vertices.size() * sizeof(vertices[0]));
			init_data.slice_pitch = 0;
			init_data.data = &vertices[0];
			GraphicsBufferPtr pos_vb = rf.MakeVertexBuffer(BU_Static, EAH_GPU_Read, &init_data);
			rl_->BindVertexStream(pos_vb, boost::make_tuple(vertex_element(VEU_Position, 0, EF_BGR32F)));

			init_data.row_pitch = static_cast<uint32_t>(indices.size() * sizeof(indices[0]));
			init_data.slice_pitch = 0;
			init_data.data = &indices[0];
			GraphicsBufferPtr ib = rf.MakeIndexBuffer(BU_Static, EAH_GPU_Read, &init_data);
			rl_->BindIndexStream(ib, EF_R16);

			std::vector<float3> normal(vertices.size());
			MathLib::compute_normal<float>(&normal[0],
				&indices[0], &indices[0] + indices.size(),
				&vertices[0], &vertices[0] + vertices.size());

			init_data.row_pitch = static_cast<uint32_t>(normal.size() * sizeof(normal[0]));
			init_data.slice_pitch = 0;
			init_data.data = &normal[0];
			GraphicsBufferPtr normal_vb = rf.MakeVertexBuffer(BU_Static, EAH_GPU_Read, &init_data);
			rl_->BindVertexStream(normal_vb, boost::make_tuple(vertex_element(VEU_Normal, 0, EF_BGR32F)));

			box_ = MathLib::compute_bounding_box<float>(&vertices[0], &vertices[0] + vertices.size());

			*(technique_->Effect().ParameterByName("grass_tex")) = grass();
		}

		void OnRenderBegin()
		{
			App3DFramework const & app = Context::Instance().AppInstance();

			float4x4 view = app.ActiveCamera().ViewMatrix();
			float4x4 proj = app.ActiveCamera().ProjMatrix();

			*(technique_->Effect().ParameterByName("View")) = view;
			*(technique_->Effect().ParameterByName("Proj")) = proj;

			Camera const & camera = Context::Instance().AppInstance().ActiveCamera();
			*(technique_->Effect().ParameterByName("depth_min")) = camera.NearPlane();
			*(technique_->Effect().ParameterByName("inv_depth_range")) = 1 / (camera.FarPlane() - camera.NearPlane());
		}
	};

	class TerrainObject : public SceneObjectHelper
	{
	public:
		TerrainObject(std::vector<float3> const & vertices, std::vector<uint16_t> const & indices)
			: SceneObjectHelper(RenderablePtr(new TerrainRenderable(vertices, indices)), SOA_Cullable)
		{
		}
	};

	int const NUM_PARTICLE = 8192;

	class RenderParticles : public RenderableHelper
	{
	public:
		RenderParticles()
			: RenderableHelper(L"Particles")
		{
			TextureLoader particle_tl = LoadTexture("particle.dds", EAH_GPU_Read);

			RenderFactory& rf = Context::Instance().RenderFactoryInstance();

			float2 texs[] =
			{
				float2(0.0f, 0.0f),
				float2(1.0f, 0.0f),
				float2(0.0f, 1.0f),
				float2(1.0f, 1.0f)
			};

			uint16_t indices[] =
			{
				0, 1, 2, 3
			};

			rl_ = rf.MakeRenderLayout();
			if (use_gs)
			{
				rl_->TopologyType(RenderLayout::TT_PointList);

				GraphicsBufferPtr pos_vb = rf.MakeVertexBuffer(BU_Dynamic, EAH_GPU_Read | EAH_CPU_Write, NULL);
				rl_->BindVertexStream(pos_vb, boost::make_tuple(vertex_element(VEU_Position, 0, EF_ABGR32F)));

				technique_ = rf.LoadEffect("ParticleSystem.kfx")->TechniqueByName("ParticleWithGS");
			}
			else
			{
				rl_->TopologyType(RenderLayout::TT_TriangleStrip);

				ElementInitData init_data;
				init_data.row_pitch = sizeof(texs);
				init_data.slice_pitch = 0;
				init_data.data = texs;
				GraphicsBufferPtr tex_vb = rf.MakeVertexBuffer(BU_Static, EAH_GPU_Read, &init_data);
				rl_->BindVertexStream(tex_vb, boost::make_tuple(vertex_element(VEU_Position, 0, EF_GR32F)));

				GraphicsBufferPtr pos_vb = rf.MakeVertexBuffer(BU_Dynamic, EAH_GPU_Read | EAH_CPU_Write, NULL);
				rl_->BindVertexStream(pos_vb,
					boost::make_tuple(vertex_element(VEU_TextureCoord, 0, EF_ABGR32F)),
					RenderLayout::ST_Instance);

				init_data.row_pitch = sizeof(indices);
				init_data.slice_pitch = 0;
				init_data.data = indices;
				GraphicsBufferPtr ib = rf.MakeIndexBuffer(BU_Static, EAH_GPU_Read, &init_data);
				rl_->BindIndexStream(ib, EF_R16);

				technique_ = rf.LoadEffect("ParticleSystem.kfx")->TechniqueByName("Particle");
			}

			*(technique_->Effect().ParameterByName("point_radius")) = 0.04f;
			*(technique_->Effect().ParameterByName("particle_tex")) = particle_tl();
		}

		void SceneTexture(TexturePtr tex, bool flip)
		{
			*(technique_->Effect().ParameterByName("scene_tex")) = tex;
			*(technique_->Effect().ParameterByName("flip")) = flip ? -1 : 1;
		}

		void OnRenderBegin()
		{
			Camera const & camera = Context::Instance().AppInstance().ActiveCamera();

			float4x4 const & view = camera.ViewMatrix();
			float4x4 const & proj = camera.ProjMatrix();
			float4x4 const inv_proj = MathLib::inverse(proj);

			*(technique_->Effect().ParameterByName("View")) = view;
			*(technique_->Effect().ParameterByName("Proj")) = proj;

			RenderEngine& re = Context::Instance().RenderFactoryInstance().RenderEngineInstance();
			float4 const & texel_to_pixel = re.TexelToPixelOffset() * 2;
			float const x_offset = texel_to_pixel.x() / re.CurFrameBuffer()->Width();
			float const y_offset = texel_to_pixel.y() / re.CurFrameBuffer()->Height();
			*(technique_->Effect().ParameterByName("offset")) = float2(x_offset, y_offset);

			*(technique_->Effect().ParameterByName("upper_left")) = MathLib::transform_coord(float3(-1, 1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("upper_right")) = MathLib::transform_coord(float3(1, 1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("lower_left")) = MathLib::transform_coord(float3(-1, -1, 1), inv_proj);
			*(technique_->Effect().ParameterByName("lower_right")) = MathLib::transform_coord(float3(1, -1, 1), inv_proj);

			*(technique_->Effect().ParameterByName("depth_min")) = camera.NearPlane();
			*(technique_->Effect().ParameterByName("inv_depth_range")) = 1 / (camera.FarPlane() - camera.NearPlane());
		}
	};

	class ParticlesObject : public SceneObjectHelper
	{
	public:
		ParticlesObject()
			: SceneObjectHelper(SOA_Cullable)
		{
			renderable_.reset(new RenderParticles);
		}
	};

	class CopyPostProcess : public PostProcess
	{
	public:
		CopyPostProcess()
			: PostProcess(Context::Instance().RenderFactoryInstance().LoadEffect("ParticleSystem.kfx")->TechniqueByName("Copy"))
		{
		}
	};

	enum
	{
		Exit
	};

	InputActionDefine actions[] =
	{
		InputActionDefine(Exit, KS_Escape)
	};

	bool ConfirmDevice()
	{
		RenderFactory& rf = Context::Instance().RenderFactoryInstance();
		RenderEngine& re = rf.RenderEngineInstance();
		RenderDeviceCaps const & caps = re.DeviceCaps();
		if (caps.max_shader_model < 2)
		{
			return false;
		}

		try
		{
			TexturePtr temp_tex = rf.MakeTexture2D(800, 600, 1, EF_ABGR16F, 1, 0, EAH_GPU_Read | EAH_GPU_Write, NULL);
			rf.Make2DRenderView(*temp_tex, 0);
		}
		catch (...)
		{
			return false;
		}

		return true;
	}
}


int main()
{
	ResLoader::Instance().AddPath("../Samples/media/Common");
	ResLoader::Instance().AddPath("../Samples/media/ParticleSystem");

	RenderSettings settings = Context::Instance().LoadCfg("KlayGE.cfg");
	settings.ConfirmDevice = ConfirmDevice;

	Context::Instance().SceneManagerInstance(SceneManager::NullObject());

	ParticleSystemApp app("Particle System", settings);
	app.Create();
	app.Run();

	return 0;
}

ParticleSystemApp::ParticleSystemApp(std::string const & name, RenderSettings const & settings)
					: App3DFramework(name, settings)
{
}

void ParticleSystemApp::InitObjects()
{
	use_gs = (Context::Instance().RenderFactoryInstance().RenderEngineInstance().DeviceCaps().max_shader_model >= 4);

	// ��������
	font_ = Context::Instance().RenderFactoryInstance().MakeFont("gkai00mp.kfont");

	this->LookAt(float3(-1.2f, 2.2f, -1.2f), float3(0, 0.5f, 0));
	this->Proj(0.01f, 100);

	fpcController_.AttachCamera(this->ActiveCamera());
	fpcController_.Scalers(0.05f, 0.1f);

	InputEngine& inputEngine(Context::Instance().InputFactoryInstance().InputEngineInstance());
	InputActionMap actionMap;
	actionMap.AddActions(actions, actions + sizeof(actions) / sizeof(actions[0]));

	action_handler_t input_handler(new input_signal);
	input_handler->connect(boost::bind(&ParticleSystemApp::InputHandler, this, _1, _2));
	inputEngine.ActionMap(actionMap, input_handler, true);

	height_img_.reset(new HeightImg(-2, -2, 2, 2, LoadTexture("grcanyon.dds", EAH_CPU_Read)(), 1));
	particles_.reset(new ParticlesObject);
	particles_->AddToSceneManager();

	std::vector<float3> vertices;
	std::vector<uint16_t> indices;
	HeightMap hm;
	hm.BuildTerrain(-2, -2, 2, 2, 4.0f / 64, 4.0f / 64, vertices, indices, *height_img_);
	terrain_.reset(new TerrainObject(vertices, indices));
	terrain_->AddToSceneManager();


	std::vector<float3> normal(vertices.size());
	MathLib::compute_normal<float>(&normal[0],
		&indices[0], &indices[0] + indices.size(),
		&vertices[0], &vertices[0] + vertices.size());

	ps_.reset(new ParticleSystem<Particle>(NUM_PARTICLE,
		GenParticle<Particle>(), UpdateParticle<Particle>(height_img_, normal)));

	ps_->AutoEmit(128);

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	scene_buffer_ = rf.MakeFrameBuffer();
	FrameBufferPtr screen_buffer = re.CurFrameBuffer();
	scene_buffer_->GetViewport().camera = screen_buffer->GetViewport().camera;

	copy_pp_.reset(new CopyPostProcess);
}

void ParticleSystemApp::OnResize(uint32_t width, uint32_t height)
{
	App3DFramework::OnResize(width, height);

	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	//RenderEngine& re = rf.RenderEngineInstance();

	RenderViewPtr ds_view = rf.MakeDepthStencilRenderView(width, height, EF_D16, 1, 0);

	scene_tex_ = rf.MakeTexture2D(width, height, 1, EF_ABGR16F, 1, 0, EAH_GPU_Read | EAH_GPU_Write, NULL);
	scene_buffer_->Attach(FrameBuffer::ATT_Color0, rf.Make2DRenderView(*scene_tex_, 0));
	scene_buffer_->Attach(FrameBuffer::ATT_DepthStencil, ds_view);

	checked_pointer_cast<RenderParticles>(particles_->GetRenderable())->SceneTexture(scene_tex_, scene_buffer_->RequiresFlipping());

	copy_pp_->Source(scene_tex_, scene_buffer_->RequiresFlipping());
	copy_pp_->Destinate(FrameBufferPtr());
}

void ParticleSystemApp::InputHandler(InputEngine const & /*sender*/, InputAction const & action)
{
	switch (action.first)
	{
	case Exit:
		this->Quit();
		break;
	}
}

class particle_cmp
{
public:
	bool operator()(std::pair<Particle, float> const & lhs, std::pair<Particle, float> const & rhs) const
	{
		return lhs.second > rhs.second;
	}
};

uint32_t ParticleSystemApp::DoUpdate(uint32_t pass)
{
	RenderFactory& rf = Context::Instance().RenderFactoryInstance();
	RenderEngine& re = rf.RenderEngineInstance();

	switch (pass)
	{
	case 0:
		re.BindFrameBuffer(scene_buffer_);
		re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Color | FrameBuffer::CBM_Depth, Color(0.2f, 0.4f, 0.6f, 1), 1.0f, 0);

		terrain_->Visible(true);
		particles_->Visible(false);
		return App3DFramework::URV_Need_Flush;

	default:
		re.BindFrameBuffer(FrameBufferPtr());
		re.CurFrameBuffer()->Clear(FrameBuffer::CBM_Depth, Color(0, 0, 0, 0), 1, 0);

		copy_pp_->Apply();

		float4x4 mat = MathLib::rotation_x(PI / 6) * MathLib::rotation_y(clock() / 300.0f) * MathLib::translation(0.0f, 0.8f, 0.0f);
		ps_->ModelMatrix(mat);

		ps_->Update(static_cast<float>(timer_.elapsed()));
		timer_.restart();

		float4x4 view_mat = Context::Instance().AppInstance().ActiveCamera().ViewMatrix();
		std::vector<std::pair<Particle, float> > active_particles;
		for (uint32_t i = 0; i < ps_->NumParticles(); ++ i)
		{
			if (ps_->GetParticle(i).life > 0)
			{
				float3 pos = ps_->GetParticle(i).pos;
				float p_to_v = (pos.x() * view_mat(0, 2) + pos.y() * view_mat(1, 2) + pos.z() * view_mat(2, 2) + view_mat(3, 2))
					/ (pos.x() * view_mat(0, 3) + pos.y() * view_mat(1, 3) + pos.z() * view_mat(2, 3) + view_mat(3, 3));

				active_particles.push_back(std::make_pair(ps_->GetParticle(i), p_to_v));
			}
		}
		if (!active_particles.empty())
		{
			std::sort(active_particles.begin(), active_particles.end(), particle_cmp());

			uint32_t const num_pars = static_cast<uint32_t>(active_particles.size());
			RenderLayoutPtr rl = particles_->GetRenderable()->GetRenderLayout();
			GraphicsBufferPtr instance_gb;
			if (use_gs)
			{
				instance_gb = rl->GetVertexStream(0);
			}
			else
			{
				instance_gb = rl->InstanceStream();

				for (uint32_t i = 0; i < rl->NumVertexStreams(); ++ i)
				{
					rl->VertexStreamFrequencyDivider(i, RenderLayout::ST_Geometry, num_pars);
				}
			}

			instance_gb->Resize(sizeof(float4) * num_pars);
			{
				GraphicsBuffer::Mapper mapper(*instance_gb, BA_Write_Only);
				float4* instance_data = mapper.Pointer<float4>();
				for (uint32_t i = 0; i < num_pars; ++ i, ++ instance_data)
				{
					instance_data->x() = active_particles[i].first.pos.x();
					instance_data->y() = active_particles[i].first.pos.y();
					instance_data->z() = active_particles[i].first.pos.z();
					instance_data->w() = active_particles[i].first.life;
				}
			}

			particles_->Visible(true);
		}
		terrain_->Visible(false);

		std::wostringstream stream;
		stream << this->FPS();

		font_->RenderText(0, 0, Color(1, 1, 0, 1), L"Particle System", 16);
		font_->RenderText(0, 18, Color(1, 1, 0, 1), stream.str(), 16);

		return App3DFramework::URV_Need_Flush | App3DFramework::URV_Finished;
	}
}
