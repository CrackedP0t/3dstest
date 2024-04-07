#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "vshader_shbin.h"
#include "emotes110_t3x.h"
#include "emotes64_t3x.h"

#define max(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a > _b ? _a : _b;       \
})

#define min(a,b)             \
({                           \
    __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b;       \
})


const int CLEAR_COLOR = 0x0437F2FF;

#define DISPLAY_TRANSFER_FLAGS                                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {float x; float y; float z; float u; float v;} vertex;
typedef struct {float x; float y; float z; float velocity_x; float velocity_y; size_t t3x_index;} spriteinfo;

static DVLB_s *vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static int uLoc_tint;
static int uLoc_depthinfo;
static C3D_Mtx projection;

static vertex *vbo_data;

static bool largetex = true;

static C3D_Tex texture_110;
static Tex3DS_Texture t3x_110;
static C3D_Tex texture_64;
static Tex3DS_Texture t3x_64;

#define MAX_SPRITES (1500)
#define SPRITE_HEIGHT (64.0f)
#define SPRITE_WIDTH (64.0f)
#define MIN_DEPTH (-25.0f)
#define MAX_DEPTH (10.0f)
#define DEEPNESS (MAX_DEPTH - MIN_DEPTH)


static int current_sprites = 1;
static spriteinfo sprites[MAX_SPRITES];

// Helper function for loading a texture from memory
static bool loadTextureFromMem(C3D_Tex *tex, Tex3DS_Texture *t3x, C3D_TexCube *cube, const void *data, size_t size)
{
	*t3x = Tex3DS_TextureImport(data, size, tex, cube, false);
	if (!*t3x)
		return false;

	return true;
}

float randbetween(float min, float max) {
	return (float)rand() / RAND_MAX * (max - min) + min;
}

void add_rect(vertex *dest, float x, float y, float z, float width, float height, const Tex3DS_SubTexture *ts) {
	vertex vertex_list[] = {
		{x, y, z, ts->left, ts->top},
		{x + width, y, z, ts->right, ts->top},
		{x, y + height, z, ts->left, ts->bottom},
		{x, y + height, z, ts->left, ts->bottom},
		{x + width, y, z, ts->right, ts->top},
		{x + width, y + height, z, ts->right, ts->bottom},
	};

	memcpy(dest, vertex_list, sizeof(vertex_list));
}

void move_rect(vertex *dest, float x, float y, float z, float width, float height) {
	dest[0].x = x;
	dest[0].y = y;
	dest[0].z = z;
	dest[1].x = x + width;
	dest[1].y = y;
	dest[1].z = z;
	dest[2].x = x;
	dest[2].y = y + height;
	dest[2].z = z;
	dest[3].x = x;
	dest[3].y = y + height;
	dest[3].z = z;
	dest[4].x = x + width;
	dest[4].y = y;
	dest[4].z = z;
	dest[5].x = x + width;
	dest[5].y = y + height;
	dest[5].z = z;
}

void uv_rect(vertex *dest, const Tex3DS_SubTexture *ts) {
	dest[0].u = ts->left;
	dest[0].v = ts->top;
	dest[1].u = ts->right;
	dest[1].v = ts->top;
	dest[2].u = ts->left;
	dest[2].v = ts->bottom;
	dest[3].u = ts->left;
	dest[3].v = ts->bottom;
	dest[4].u = ts->right;
	dest[4].v = ts->top;
	dest[5].u = ts->right;
	dest[5].v = ts->bottom;
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
	uLoc_depthinfo = shaderInstanceGetUniformLocation(program.vertexShader, "depthinfo");

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo *attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

	// Compute the projection matrix
	Mtx_OrthoTilt(&projection, 0, 400.0, 240, 0, 1000.0, -1000.0, true);

	// Load the texture and bind it to the first texture unit
	if (!loadTextureFromMem(&texture_110, &t3x_110, NULL, emotes110_t3x, emotes110_t3x_size))
		svcBreak(USERBREAK_PANIC);
		
	if (!loadTextureFromMem(&texture_64, &t3x_64, NULL, emotes64_t3x, emotes64_t3x_size))
		svcBreak(USERBREAK_PANIC);

	// Create the VBO (vertex buffer object)
	vbo_data = linearAlloc(MAX_SPRITES * 6 * sizeof(vertex));

	for (int i = 0; i < MAX_SPRITES; i++) {
		float width = 400 - SPRITE_WIDTH;
		float height = 240 - SPRITE_HEIGHT;

		spriteinfo s =  {randbetween(0, width), randbetween(0, height), randbetween(MIN_DEPTH, MAX_DEPTH), randbetween(-2, 2), randbetween(-2, 2),  randbetween(0, Tex3DS_GetNumSubTextures(t3x_110))};
		sprites[i] = s;
		
		const Tex3DS_SubTexture *ts = Tex3DS_GetSubTexture(largetex ? t3x_110 : t3x_64, s.t3x_index);

		// add_rect(&vbo_data[i * 6], s.x, s.y, MIN_DEPTH + (float)(i + 1) / (float)MAX_SPRITES * DEEPNESS, SPRITE_WIDTH, SPRITE_HEIGHT, ts);
		add_rect(&vbo_data[i * 6], s.x, s.y, s.z, SPRITE_WIDTH, SPRITE_HEIGHT, ts);
	}

	// Configure buffers
	C3D_BufInfo *bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	// C3D_TexSetWrap(&texture, GPU_REPEAT, GPU_REPEAT);
	C3D_TexSetFilter(&texture_110, GPU_LINEAR, GPU_NEAREST);
	C3D_TexSetFilter(&texture_64, GPU_LINEAR, GPU_NEAREST);
	C3D_TexBind(0, &texture_110);
	// Configure the first fragment shading substage to blend the texture color with
	// the vertex color (calculated by the vertex shader using a lighting algorithm)
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

	C3D_AlphaTest(true, GPU_EQUAL, 255);

	C3D_CullFace(GPU_CULL_NONE);
}

static void update(float delta) {
	delta *= 6.0 / 100.0;
	for (int i = 0; i < current_sprites; i++) {
		spriteinfo *s = &sprites[i];
		s->x += s->velocity_x * delta;
		s->y += s->velocity_y * delta;

		move_rect(&vbo_data[i * 6], s->x, s->y, s->z, SPRITE_WIDTH, SPRITE_HEIGHT);

		if (s->x < 0 || s->x + SPRITE_WIDTH > (float)GSP_SCREEN_HEIGHT_TOP) {
			s->velocity_x *= -1;
		}
		if (s->y < 0 || s->y + SPRITE_HEIGHT > (float)GSP_SCREEN_WIDTH) {
			s->velocity_y *= -1;
		}
	}
}

static void sceneRender(float iod)
{
	// Update the uniforms
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_tint, 1.0f, 1.0f, 1.0f, 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_depthinfo, iod, MIN_DEPTH, MAX_DEPTH, DEEPNESS);

	// Draw the VBO
	C3D_DrawArrays(GPU_TRIANGLES, 0, current_sprites * 6);
}

static void sceneExit(void)
{
	// Free the texture
	C3D_TexDelete(&texture_110);

	// Free the VBO
	linearFree(vbo_data);

	// Free the shader program
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

static bool paused = false;

int main()
{
	osSetSpeedupEnable(true);

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
		C3D_FrameBegin(/*C3D_FRAME_SYNCDRAW*/0);

		hidScanInput();

		float iod = osGet3DSliderState();

		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
		if (kDown & KEY_SELECT)
			paused = !paused;
			
		u32 kHeld = hidKeysHeld();
		if ((kHeld & KEY_UP) && current_sprites < MAX_SPRITES)
			current_sprites++;
		if ((kHeld & KEY_DOWN) && current_sprites > 1)
			current_sprites--;
			
		if ((kDown & KEY_RIGHT) && current_sprites)
			current_sprites = min(current_sprites + 100, MAX_SPRITES);
		if (kDown & KEY_LEFT)
			current_sprites = max(current_sprites - 100, 1);

		if (kDown & KEY_X) {
			largetex = !largetex;
			if (largetex) {
				C3D_TexBind(0, &texture_110);
			} else {
				C3D_TexBind(0, &texture_64);
			}
			for (int i = 0; i < MAX_SPRITES; i++) {
				spriteinfo *s = &sprites[i];
				const Tex3DS_SubTexture *ts = Tex3DS_GetSubTexture(largetex ? t3x_110 : t3x_64, s->t3x_index);
				uv_rect(&vbo_data[i * 6], ts);
			}
		}

		osTickCounterUpdate(&counter);
		double frametime = osTickCounterRead(&counter);

		if (!paused)
			update(frametime);

		C3D_RenderTargetClear(left_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(left_target);
		sceneRender(iod);
		if (iod > 0.0f) {
			C3D_RenderTargetClear(right_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
			C3D_FrameDrawOn(right_target);
			sceneRender(-iod);
		}
		C3D_FrameEnd(0);

		printf("\x1b[1;1H  Sprites: %zu/%u\x1b[K", current_sprites, MAX_SPRITES);
		printf("\x1b[2;1H      CPU: %.2fms\x1b[K", C3D_GetProcessingTime());
		printf("\x1b[3;1H      GPU: %.2fms\x1b[K", C3D_GetDrawingTime());
		printf("\x1b[4;1H   CmdBuf: %.2f%%\x1b[K", C3D_GetCmdBufUsage()*100.0f);
		printf("\x1b[5;1HFrametime: %.2fms\x1b[K", frametime);
		printf("\x1b[6;1H      FPS: %.2f\x1b[K", 1.0 / frametime * 1000.0);
	}

	// Deinitialize the scene
	sceneExit();

	// Deinitialize graphics
	C3D_Fini();
	gfxExit();
	return 0;
}
