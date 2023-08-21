#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "vshader_shbin.h"
#include "raysQ_256_t3x.h"
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
static int uLoc_side;
static C3D_Mtx projection;

static vertex *vbo_data;
static C3D_Tex texture;

#define MAX_SPRITES (15000)
#define SPRITE_HEIGHT (64.0f)
#define SPRITE_WIDTH (64.0f)
#define ORIGINAL_WIDTH (256.0)
#define ORIGINAL_HEIGHT (256.0)

static int current_sprites = 10;
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

void add_rect(vertex * dest, float x, float y, float z, float width, float height) {
	float u =  (float)ORIGINAL_WIDTH / (float)texture.width;
	float v =  1.0 - (float)ORIGINAL_HEIGHT / (float)texture.height;

	vertex vertex_list[] = {
		{{x, y, z}, {0.0, 1.0}},
		{{x + width, y, z}, {u, 1.0}},
		{{x, y + height, z}, {0.0, v}},
		{{x, y + height, z}, {0.0, v}},
		{{x + width, y, z}, {u, 1.0}},
		{{x + width, y + height, z}, {u, v}},
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
	uLoc_side = shaderInstanceGetUniformLocation(program.vertexShader, "side");

	// Configure attributes for use with the vertex shader
	C3D_AttrInfo *attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 3); // v0=position
	AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);

	// Compute the projection matrix
	Mtx_OrthoTilt(&projection, 0, 400.0, 240, 0, 1000.0, -1000.0, true);

	// Load the texture and bind it to the first texture unit
	if (!loadTextureFromMem(&texture, NULL, raysQ_256_t3x, raysQ_256_t3x_size))
		svcBreak(USERBREAK_PANIC);

	// Create the VBO (vertex buffer object)
	vbo_data = linearAlloc(MAX_SPRITES * 6 * sizeof(vertex));

	for (int i = 0; i < MAX_SPRITES; i++) {
		float width = 400 - SPRITE_WIDTH;
		float height = 240 - SPRITE_HEIGHT;

		float x = randbetween(0, width);
		float y = randbetween(0, height);

		add_rect(&vbo_data[i * 6], x, y, randbetween(-20.0, -1), SPRITE_WIDTH, SPRITE_HEIGHT);

		spriteinfo info =  {randbetween(-2, 2), randbetween(-2, 2)};
		sprites[i] = info;
	}

	// Configure buffers
	C3D_BufInfo *bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, vbo_data, sizeof(vertex), 2, 0x10);

	// C3D_TexSetWrap(&texture, GPU_REPEAT, GPU_REPEAT);
	C3D_TexSetFilter(&texture, GPU_LINEAR, GPU_NEAREST);
	C3D_TexBind(0, &texture);
	// Configure the first fragment shading substage to blend the texture color with
	// the vertex color (calculated by the vertex shader using a lighting algorithm)
	// See https://www.opengl.org/sdk/docs/man2/xhtml/glTexEnv.xml for more insight
	C3D_TexEnv *env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

	C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	C3D_CullFace(GPU_CULL_NONE);
}

void printvec(C3D_FVec v) {
	printf("(%f, %f, %f, %f)\n", v.x, v.y, v.z, v.w);
}

static void update(float delta) {
	for (int i = 0; i < current_sprites; i++) {
		for (int j = 0; j < 6; j++) {
			vertex *v = &vbo_data[i * 6 + j];
			v->position[0] += sprites[i].velocity_x;
			v->position[1] += sprites[i].velocity_y;
		}
		vertex *v = &vbo_data[i * 6];
		if (v->position[0] < 0 || v->position[0] + SPRITE_WIDTH > (float)GSP_SCREEN_HEIGHT_TOP) {
			sprites[i].velocity_x *= -1;
		}
		if (v->position[1] < 0 || v->position[1] + SPRITE_HEIGHT > (float)GSP_SCREEN_WIDTH) {
			sprites[i].velocity_y *= -1;
		}
	}
}

static void sceneRender(float side)
{
	// Update the uniforms
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_tint, 1.0f, 1.0f, 1.0f, 1.0f);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc_side, side, side, side, side);

	// Draw the VBO
	C3D_DrawArrays(GPU_TRIANGLES, 0, current_sprites * 6);
}

static void sceneExit(void)
{
	// Free the texture
	C3D_TexDelete(&texture);

	// Free the VBO
	linearFree(vbo_data);

	// Free the shader program
	shaderProgramFree(&program);
	DVLB_Free(vshader_dvlb);
}

static bool paused = false;

int main()
{
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

	// Main loop
	while (aptMainLoop())
	{
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
			
		if ((kHeld & KEY_RIGHT) && current_sprites)
			current_sprites = min(current_sprites + 100, MAX_SPRITES);
		if (kHeld & KEY_LEFT)
			current_sprites = max(current_sprites - 100, 1);


		TickCounter start;
		osTickCounterStart(&start);

		// Render the scene
		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

		if (!paused)
			update(1.0);

		printf("\n\n\n\n\n\n\n%f\n", iod);
		
		C3D_RenderTargetClear(left_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(left_target);
		sceneRender(iod);
		C3D_RenderTargetClear(right_target, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
		C3D_FrameDrawOn(right_target);
		sceneRender(-iod);
		C3D_FrameEnd(0);
		
		osTickCounterUpdate(&start);
		double frametime = osTickCounterRead(&start);

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
