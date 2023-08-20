#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "vshader_shbin.h"
#include "raysQ_t3x.h"
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


#define CLEAR_COLOR 0x68B0D8FF

#define DISPLAY_TRANSFER_FLAGS                                                                     \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |               \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

typedef struct {float position[3]; float texcoord[2];} vertex;
typedef struct {float velocity_x; float velocity_y;} spriteinfo;

static DVLB_s *vshader_dvlb;
static shaderProgram_s program;
static int uLoc_projection;
static int uLoc_tint;
static C3D_Mtx projection;

static vertex *vbo_data;
static C3D_Tex raysQ_tex;

#define MAX_SPRITES (15000)
#define SPRITE_HEIGHT (50.0f)
#define SPRITE_WIDTH (50.0f)

static int current_sprites = 2000;
static spriteinfo sprites[MAX_SPRITES];

// Helper function for loading a texture from memory
static bool loadTextureFromMem(C3D_Tex *tex, C3D_TexCube *cube, const void *data, size_t size)
{
	Tex3DS_Texture t3x = Tex3DS_TextureImport(data, size, tex, cube, false);
	if (!t3x)
		return false;

	// Delete the t3x object since we don't need it
	Tex3DS_TextureFree(t3x);
	return true;
}

float randbetween(float min, float max) {
	return (float)rand() / RAND_MAX * (max - min) + min;
}

void add_rect(vertex * dest, float x, float y, float width, float height) {
	vertex vertex_list[] = {
		{{x, y, 1.0}, {0.0, 1.0}},
		{{x + width, y, 1.0}, {1.0, 1.0}},
		{{x, y + height, 1.0}, {0.0, 0.0}},
		{{x, y + height, 1.0}, {0.0, 0.0}},
		{{x + width, y, 1.0}, {1.0, 1.0}},
		{{x + width, y + height, 1.0}, {1.0, 0.0}},
	};

	memcpy(dest, vertex_list, sizeof(vertex_list));
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
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

	// Compute the projection matrix
	 Mtx_OrthoTilt(&projection, 0, 400.0, 240, 0, 0.0, 1.0, true);

	// Create the VBO (vertex buffer object)
	vbo_data = linearAlloc(MAX_SPRITES * 6 * sizeof(vertex));

	for (int i = 0; i < MAX_SPRITES; i++) {
		float width = 400 - SPRITE_WIDTH;
		float height = 240 - SPRITE_HEIGHT;

		float x = randbetween(0, width);
		float y = randbetween(0, height);

		add_rect(&vbo_data[i * 6], x, y, SPRITE_WIDTH, SPRITE_HEIGHT);

		spriteinfo info =  {randbetween(-2, 2), randbetween(-2, 2)};
		sprites[i] = info;
	}

	// Configure buffers
	C3D_BufInfo *bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	// Load the texture and bind it to the first texture unit
	if (!loadTextureFromMem(&raysQ_tex, NULL, raysQ_t3x, raysQ_t3x_size))
		svcBreak(USERBREAK_PANIC);
	C3D_TexSetFilter(&raysQ_tex, GPU_LINEAR, GPU_NEAREST);
	C3D_TexBind(0, &raysQ_tex);
	// Configure the first fragment shading substage to blend the texture color with
	// the vertex color (calculated by the vertex shader using a lighting algorithm)
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	// C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
	// C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);

	C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
}

void printvec(C3D_FVec v) {
	printf("(%f, %f, %f, %f)\n", v.x, v.y, v.z, v.w);
}

static void sceneRender(void)
{
	// Update the uniforms
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_tint, 1.0f, 1.0f, 1.0f, 1.0f);

	for (int i = 0; i < current_sprites; i++) {
		for (int j = 0; j < 6; j++) {
			vertex *v = &vbo_data[i * 6 + j];
			v->position[0] += sprites[i].velocity_x;
			v->position[1] += sprites[i].velocity_y;
		}
		vertex *v = &vbo_data[i * 6];
		if (v->position[0] < 0 || v->position[0] + SPRITE_WIDTH > GSP_SCREEN_HEIGHT_TOP) {
			sprites[i].velocity_x *= -1;
		}
		if (v->position[1] < 0 || v->position[1] + SPRITE_HEIGHT > GSP_SCREEN_WIDTH) {
			sprites[i].velocity_y *= -1;
		}
	}

	// Draw the VBO
	C3D_DrawArrays(GPU_TRIANGLES, 0, current_sprites * 6);

	//consoleClear();

	printf("\x1b[1;1HSprites: %zu/%u\x1b[K", current_sprites, MAX_SPRITES);
	printf("\x1b[2;1HCPU:     %6.2f%%\x1b[K", C3D_GetProcessingTime()*6.0f);
	printf("\x1b[3;1HGPU:     %6.2f%%\x1b[K", C3D_GetDrawingTime()*6.0f);
	printf("\x1b[4;1HCmdBuf:  %6.2f%%\x1b[K", C3D_GetCmdBufUsage()*100.0f);
}

static void sceneExit(void)
{
	// Free the texture
	C3D_TexDelete(&raysQ_tex);

	// Free the VBO
	linearFree(vbo_data);

	// Free the shader program
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

int main()
{
	// Initialize graphics
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	consoleInit(GFX_BOTTOM, NULL);

	// Initialize the render target
	C3D_RenderTarget *target = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

	// Initialize the scene
	sceneInit();

	// Main loop
	while (aptMainLoop())
	{
		hidScanInput();

		// Respond to user input
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu
			
		u32 kHeld = hidKeysHeld();
		if ((kHeld & KEY_UP) && current_sprites < MAX_SPRITES)
			current_sprites++;
		if ((kHeld & KEY_DOWN) && current_sprites > 1)
			current_sprites--;
			
		if ((kHeld & KEY_RIGHT) && current_sprites)
			current_sprites = min(current_sprites + 100, MAX_SPRITES);
		if (kHeld & KEY_LEFT)
			current_sprites = max(current_sprites - 100, 1);

		// Render the scene
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
		C3D_RenderTargetClear(target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(target);
		sceneRender();
		C3D_FrameEnd(0);
		
		clock_gettime(CLOCK_MONOTONIC, &end);
		double delta_ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
		printf("\x1b[5;1HFPS:     %6.2fms\x1b[K", delta_ms);
	}

	// Deinitialize the scene
	sceneExit();

	// Deinitialize graphics
	C3D_Fini();
	gfxExit();
	return 0;
}
