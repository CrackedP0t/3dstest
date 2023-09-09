#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <chrono>
#define GLM_MESSAGES
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/transform.hpp>
#include "vshader_shbin.h"
#include "pixelflakes_t3x.h"

#define max(a, b)               \
	({                          \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a > _b ? _a : _b;      \
	})

#define min(a, b)               \
	({                          \
		__typeof__(a) _a = (a); \
		__typeof__(b) _b = (b); \
		_a < _b ? _a : _b;      \
	})

const int CLEAR_COLOR = 0x00000000;

#define DISPLAY_TRANSFER_FLAGS                                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

struct Spriteinfo
{
public:
	glm::vec3 pos;
	glm::vec2 velocity;
	float rotation;
	float velocity_r;
	size_t t3x_index;
};

static DVLB_s *vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static int uLoc_tint;
static C3D_Mtx projection;

static glm::vec3 *position_vbo;
static glm::vec2 *uv_vbo;
static C3D_Tex texture;
static Tex3DS_Texture t3x;

static glm::vec3 camera = {0.5, 0.5, 0.0};

constexpr size_t MAX_SPRITES = 5000;
constexpr float BOX_DIM = 10.0;
constexpr float H_BOX_DIM = BOX_DIM / 2.0;
constexpr float FLAKE_DIM = 0.05;
constexpr float MAX_FLAKE_VEL = 0.005;
constexpr float CAMERA_SPEED = 0.02;

static size_t current_sprites = 2000;
static Spriteinfo sprites[MAX_SPRITES];

inline glm::mat4x4 c2g(C3D_Mtx old)
{
	glm::mat4x4 n;
	memcpy(glm::value_ptr(n), &old, sizeof old);
	return n;
}

// Helper function for loading a texture from memory
static bool loadTextureFromMem(C3D_Tex *tex, Tex3DS_Texture *t3x, C3D_TexCube *cube, const void *data, size_t size)
{
	*t3x = Tex3DS_TextureImport(data, size, tex, cube, false);

	if (!*t3x)
		return false;

	return true;
}

float randbetween(float min, float max)
{
	return (float)rand() / RAND_MAX * (max - min) + min;
}

void set_rect(size_t index, glm::vec3 pos, float rotation, float width, float height)
{
	float w2 = width / 2;
	float h2 = height / 2;

	glm::mat4x4 trans = glm::rotate(rotation, glm::vec3(0.0f, 0.0f, 1.0f));
	glm::vec3 vertex_list[] = {
		pos + (trans * glm::vec4(-w2, -h2, 0, 1.0)).xyz(),
		pos + (trans * glm::vec4(w2, -h2, 0, 1.0)).xyz(),
		pos + (trans * glm::vec4(-w2, h2, 0, 1.0)).xyz(),
		pos + (trans * glm::vec4(-w2, h2, 0, 1.0)).xyz(),
		pos + (trans * glm::vec4(w2, -h2, 0, 1.0)).xyz(),
		pos + (trans * glm::vec4(w2, h2, 0, 1.0)).xyz()};

	memcpy(position_vbo + index, vertex_list, sizeof(vertex_list));
}

void uv_rect(size_t index, const Tex3DS_SubTexture *ts)
{
	glm::vec2 uvs[] = {
		glm::vec2(ts->left, ts->top),
		glm::vec2(ts->right, ts->top),
		glm::vec2(ts->left, ts->bottom),
		glm::vec2(ts->left, ts->bottom),
		glm::vec2(ts->right, ts->top),
		glm::vec2(ts->right, ts->bottom)};

	memcpy(uv_vbo + index, uvs, sizeof(uvs));
}

static void sceneInit(void)
{
	// Load the vertex shader, create a shader program and bind it
	vshader_dvlb = DVLB_ParseFile((u32 *)vshader_shbin, vshader_shbin_size);
	shaderProgramInit(&program);
	shaderProgramSetVsh(&program, &vshader_dvlb->DVLE[0]);
	C3D_BindProgram(&program);

	// Get the location of the uniforms
	uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");
	uLoc_tint = shaderInstanceGetUniformLocation(program.vertexShader, "tint");

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo *attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3);
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

	// Load the texture and bind it to the first texture unit
	if (!loadTextureFromMem(&texture, &t3x, NULL, pixelflakes_t3x, pixelflakes_t3x_size))
		svcBreak(USERBREAK_PANIC);

	// Create the VBO (vertex buffer object)
	position_vbo = (glm::vec3 *)linearAlloc(MAX_SPRITES * 6 * sizeof(glm::vec3));
	uv_vbo = (glm::vec2 *)linearAlloc(MAX_SPRITES * 6 * sizeof(glm::vec2));

	for (size_t i = 0; i < MAX_SPRITES; i++)
	{
		glm::vec3 pos = {randbetween(-H_BOX_DIM, H_BOX_DIM), randbetween(-H_BOX_DIM, H_BOX_DIM), randbetween(-H_BOX_DIM, H_BOX_DIM)};
		Spriteinfo s = {pos, glm::vec2(randbetween(-MAX_FLAKE_VEL, MAX_FLAKE_VEL), randbetween(-MAX_FLAKE_VEL, MAX_FLAKE_VEL)), randbetween(0, M_2_PI), randbetween(-.03, .03), (size_t)randbetween(0, Tex3DS_GetNumSubTextures(t3x) - 1)};
		sprites[i] = s;

		const Tex3DS_SubTexture *ts = Tex3DS_GetSubTexture(t3x, s.t3x_index);

		set_rect(i * 6, pos, s.rotation, FLAKE_DIM, FLAKE_DIM);
		uv_rect(i * 6, ts);
	}

	// Configure buffers
	C3D_BufInfo *bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, position_vbo, sizeof(glm::vec3), 1, 0x0);
	BufInfo_Add(bufInfo, uv_vbo, sizeof(glm::vec2), 1, 0x1);

	C3D_TexSetFilter(&texture, GPU_NEAREST, GPU_NEAREST);
	C3D_TexBind(0, &texture);
	// Configure the first fragment shading substage to blend the texture color with
	// the vertex color (calculated by the vertex shader using a lighting algorithm)
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

	C3D_AlphaTest(true, GPU_EQUAL, 255);

	C3D_CullFace(GPU_CULL_NONE);
}

static void update(float delta, float totaltime)
{
	delta *= 6.0 / 100.0;
	for (size_t i = 0; i < current_sprites; i++)
	{
		Spriteinfo *s = &sprites[i];
		s->pos += glm::vec3(s->velocity, 0) * delta;

		set_rect(i * 6, s->pos, s->rotation, FLAKE_DIM, FLAKE_DIM);

		if (s->pos.x < -H_BOX_DIM || s->pos.x + FLAKE_DIM > (float)H_BOX_DIM)
		{
			s->velocity.x *= -1;
		}
		if (s->pos.y < -H_BOX_DIM || s->pos.y + FLAKE_DIM > (float)H_BOX_DIM)
		{
			s->velocity.y *= -1;
		}

		s->rotation += delta * s->velocity_r;
	}
}

static void sceneRender(float iod)
{
	Mtx_PerspStereoTilt(&projection, C3D_AngleFromDegrees(45.0f), C3D_AspectRatioTop, 0.01f, 10.0, iod, 2.0, false);

	C3D_Mtx view;
	Mtx_Identity(&view);
	Mtx_Translate(&view, -camera.x, -camera.y, -camera.z, true);
	// Mtx_Scale(&view, 1.0 / 400.0, 1.0 / 400.0, 1.0);

	Mtx_Multiply(&projection, &projection, &view);

	// Update the uniforms
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_tint, 1.0f, 1.0f, 1.0f, 1.0f);

	// Draw the VBO
	C3D_DrawArrays(GPU_TRIANGLES, 0, current_sprites * 6);
}

static void sceneExit(void)
{
	// Free the texture
	C3D_TexDelete(&texture);

	// Free the VBO
	linearFree(position_vbo);

	// Free the shader program
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

static bool paused = false;

int main()
{
	auto start = std::chrono::system_clock::now();

	srand(time(NULL));
	// Initialize graphics
	gfxInitDefault();
	gfxSet3D(true);
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);

	// Initialize the render target
	C3D_RenderTarget *left_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(left_target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
	C3D_RenderTarget *right_target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(right_target, GFX_TOP, GFX_RIGHT, DISPLAY_TRANSFER_FLAGS);

	// Initialize the scene
	sceneInit();

	TickCounter counter;
	osTickCounterStart(&counter);

	// Main loop
	while (aptMainLoop())
	{
		// Render the scene
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		hidScanInput();

		float iod = osGet3DSliderState() / 3;

		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
		if (kDown & KEY_SELECT)
			paused = !paused;

		u32 kHeld = hidKeysHeld();
		if ((kHeld & KEY_R) && current_sprites < MAX_SPRITES)
			current_sprites++;
		if ((kHeld & KEY_L) && current_sprites > 1)
			current_sprites--;

		// if ((kDown & KEY_RIGHT) && current_sprites)
		// 	current_sprites = min(current_sprites + 100, MAX_SPRITES);
		// if (kDown & KEY_LEFT)
		// 	current_sprites = max(current_sprites - 100, 1);

		if (kHeld & KEY_CPAD_UP)
			camera.y += CAMERA_SPEED;
		if (kHeld & KEY_CPAD_DOWN)
			camera.y -= CAMERA_SPEED;
		if (kHeld & KEY_CPAD_LEFT)
			camera.x -= CAMERA_SPEED;
		if (kHeld & KEY_CPAD_RIGHT)
			camera.x += CAMERA_SPEED;
		if (kHeld & KEY_A)
			camera.z -= CAMERA_SPEED;
		if (kHeld & KEY_B)
			camera.z += CAMERA_SPEED;

		osTickCounterUpdate(&counter);
		double frametime = osTickCounterRead(&counter);

		float totaltime = std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(std::chrono::system_clock::now() - start).count();

		if (!paused)
			update(frametime, totaltime);

		C3D_RenderTargetClear(left_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(left_target);
		sceneRender(-iod);
		if (iod > 0.0f)
		{
			C3D_RenderTargetClear(right_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(right_target);
			sceneRender(iod);
		}
		C3D_FrameEnd(0);

		printf("\x1b[1;1H  Sprites: %zu/%u\x1b[K", current_sprites, MAX_SPRITES);
		printf("\x1b[2;1H      CPU: %.2fms\x1b[K", C3D_GetProcessingTime());
		printf("\x1b[3;1H      GPU: %.2fms\x1b[K", C3D_GetDrawingTime());
		printf("\x1b[4;1H   CmdBuf: %.2f%%\x1b[K", C3D_GetCmdBufUsage() * 100.0f);
		printf("\x1b[5;1HFrametime: %.2fms\x1b[K", frametime);
		printf("\x1b[6;1H      FPS: %.2f\x1b[K", 1.0 / frametime * 1000.0);
		printf("\x1b[7;1H   Camera: %.2f, %.2f, %.2f\x1b[K", camera.x, camera.y, camera.z);
		printf("\x1b[8;1H   Uptime: %.2fms\x1b[K", totaltime);
	}

	// Deinitialize the scene
	sceneExit();

	// Deinitialize graphics
	C3D_Fini();
	gfxExit();
	return 0;
}
