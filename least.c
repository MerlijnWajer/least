#include <SDL/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include "mupdf/fitz/fitz.h"
#include "mupdf/pdf/mupdf.h"

#include <unistd.h>

static float
    w, h,           /* Window dimensions globals */
    gl_w, gl_h;     /* GL Backbuffer dimensions globals */

SDL_Surface *surface;

float imw, imh;
GLuint *pages;
unsigned int pagec;

static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type);
static int page_to_texture(fz_context *ctx, fz_document *doc, int pagenum);
static void draw_screen(void);
static void toggle_fullscreen(void);

static float scroll = 0.0f;
static int autoscroll = 0;
static int autoscroll_var = 1;

static int redraw = 1; /* Window dirty? */

static int fullscreen = 0; /* Are fullscreen? */
static int prev_w, prev_h; /* W, H before fullscreen */

/* Set to 1 if the machine does not support NPOT textures */
static int power_of_two = 0;

/* Set to 1 to force use of POT mechanism */
static const int force_power_of_two = 0;

int open_pdf(fz_context *context, char *filename) {
    fz_stream *file;
    fz_document *doc;
    int faulty;
    unsigned int i;
    /* char * s; */

    faulty = 0;

    /* Resolution */

    printf("Opening: %s\n", filename);

    fz_try(context) {
        file = fz_open_file(context, filename);

        doc = (fz_document *) pdf_open_document_with_stream(file);

        /* TODO Password */

        fz_close(file);
    } fz_catch (context) {
        fprintf(stderr, "Cannot open: %s\n", filename);
        faulty = 1;
    }

    if (faulty)
        return faulty;

    /* XXX error handling */
    pagec = fz_count_pages(doc);
    pages = malloc(sizeof(unsigned int) * pagec);

    for(i = 0; i < pagec; i++) {
        page_to_texture(context, doc, i);
    }


    fz_close_document(doc);


    printf("Done opening\n");
    return 0;
}

static int page_to_texture(fz_context *context, fz_document *doc, int pagenum) {
    fz_page *page;
    fz_pixmap *image;
    fz_device *dev;
    fz_rect bounds;
    fz_bbox bbox;
    fz_matrix ctm;
    float scale;

    printf("Rendering page %d\n", pagenum);
    page = fz_load_page(doc, pagenum);

    bounds = fz_bound_page(doc, page);

    scale = w / bounds.x1;
    printf("W, H: (%f, %f)\n", w, h);
    printf("Scale: %f\n", scale);

    ctm = fz_scale(scale, scale);

    bounds.x1 *= scale;
    bounds.y1 *= scale;
    bbox = fz_round_rect(bounds);
    printf("Size: (%d, %d)\n", bbox.x1, bbox.y1);

    image = fz_new_pixmap_with_bbox(context, fz_device_rgb, bbox);
    dev = fz_new_draw_device(context, image);

    fz_clear_pixmap_with_value(context, image, 255);
    fz_run_page(doc, page, dev, ctm, NULL);

    /* Draw onto pixmap here */
    pages[pagenum] = pixmap_to_texture((void*)fz_pixmap_samples(context, image),
            fz_pixmap_width(context, image),
            fz_pixmap_height(context, image), 0, 0);


    fz_drop_pixmap(context, image);

    fz_free_device(dev);

    fz_free_page(doc, page);

    return pages[pagenum];
}

#if 0
#define DEBUG_GL(STR) \
    printf("OpenGL error " #STR ": %s\n", gluErrorString(glGetError()))
#else
#define DEBUG_GL(STR)
#endif

/* Get next power of 2 */
#define NPOW2(D, S) \
    { \
        D = S; \
        D |= D >> 1; \
        D |= D >> 2; \
        D |= D >> 4; \
        D |= D >> 8; \
        D |= D >> 16; \
        D++; \
    }

/* Round up to power of 2 */
#define RPOW2(D, S) \
    { \
        NPOW2(D, S); \
        D = D == S << 1 ? S : D; \
    }

static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type)
{
    unsigned int texname;
    /* int max_texsize; */

    int pow2_width, pow2_height;

    /* Compute POT texture dimensions */
    RPOW2(pow2_width, width);
    RPOW2(pow2_height, height);

    format = 42;
    type = 31337;

    glGenTextures(1, &texname);
    printf("Generated texture: %d\n", texname);
    DEBUG_GL(glGenTextures);

    /* Bind the texture object */
    glBindTexture(GL_TEXTURE_2D, texname);
    DEBUG_GL(glBindTexture);

    /* Set the texture's stretching properties */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);
    DEBUG_GL(glTexParameteri);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);
    DEBUG_GL(glTexParameteri);

    /* Edit the texture object's image data using the information SDL_Surface gives us */
    /*
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texsize);
    printf("Max tex dimensions: %dx%d\n", max_texsize, max_texsize);
    printf("width: %d, heigth %d\n", width, height);
    */

    /* Special treatment is only needed if the GPU does not support NPOT
     * textures and the current pixmap is not of POT dimensions.
     */
    if (power_of_two && (pow2_width != width || pow2_height != height)) {
        /*printf("pow2_width: %d, pow2_heigth %d\n", pow2_width, pow2_height); */

        /* Allocate undefined POT texture */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pow2_width,
                 pow2_height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 NULL);
        DEBUG_GL(glTexImage2D);

        /* Now fill texture */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
            GL_RGBA, GL_UNSIGNED_BYTE, pixmap);
        DEBUG_GL(glTexSubImage2D);

    } else {
        /* NPOT, simply pass pixmap */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width,
                 height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 pixmap);
        DEBUG_GL(glTexImage2D);
    }

    /* XXX: This function currently sets the global page size */
    imw = width;
    imh = height;

    return texname;
}

static void quit_tutorial(int code)
{
	glDeleteTextures(pagec, pages);

	exit(code);
}

static void handle_key_down(SDL_keysym * keysym)
{
	switch (keysym->sym) {
	case SDLK_ESCAPE:
		quit_tutorial(0);
		break;

    case SDLK_DOWN:
        if (autoscroll) {
            autoscroll_var += 1;
        } else {
            scroll -= 42.;
        }
        redraw = 1;
        break;

    case SDLK_UP:
        if (autoscroll) {
            autoscroll_var -= 1;
        } else {
            scroll += 42.;
        }
        redraw = 1;
        break;

    case SDLK_PAGEDOWN:
        scroll -= imh + 20;
        redraw = 1;
        break;

    case SDLK_PAGEUP:
        scroll += imh + 20;
        redraw = 1;
        break;

    case SDLK_F11:
        SDL_WM_ToggleFullScreen(surface);
        toggle_fullscreen();
        break;

    case SDLK_F12:
        if (autoscroll)
            autoscroll = 0;
        else
            autoscroll = 1;
        redraw = 1;
        break;

	default:
		break;
	}
}

static void toggle_fullscreen(void) {
	const SDL_VideoInfo *info = NULL;
	info = SDL_GetVideoInfo();

	if (!info) {
        puts("Oops - can't get video info");
    }

    if (!fullscreen) {
        fullscreen = 1;

        prev_w = w;
        prev_h = h;

        w = info->current_w;
        h = info->current_h;
    } else {
        fullscreen = 0;
        w = prev_w;
        h = prev_h;
    }


    redraw = 1;
}

static void handle_resize(SDL_ResizeEvent e) {
    /* printf("Resized to (%d, %d)\n", e.w, e.h); */
    w = e.w;
    h = e.h;

    redraw = 1;


    return;
}

static void setup_opengl(int width, int height)
{
	/* float ratio = (float)width / (float)height; */

	/* Our shading model--Gouraud (smooth). */
	glShadeModel(GL_SMOOTH);

	glEnable(GL_TEXTURE_2D);

	/* Set the clear color. */
	glClearColor(0, 0, 0, 0);

	/* Setup our viewport. */
	glViewport(0, 0, width, height);

	glLoadIdentity();
}

int setup_sdl(void)
{
	/* Information about the current video settings. */
	const SDL_VideoInfo *info = NULL;

	/* Color depth in bits of our window. */
	int bpp = 0;

	/* Flags we will pass into SDL_SetVideoMode. */
    int flags = 0;

	/* First, initialize SDL's video subsystem. */
	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		/* Failed, exit. */
		fprintf(stderr, "Video initialization failed: %s\n",
			SDL_GetError());
		quit_tutorial(1);
	}

	/* Let's get some video information. */
	info = SDL_GetVideoInfo();

	if (!info) {
		/* This should probably never happen. */
		fprintf(stderr, "Video query failed: %s\n", SDL_GetError());
		quit_tutorial(1);
	}

    /* Store current width and height, and more importantly
     * store GL backbuffer size
     */
    gl_w = w = info->current_w;
    gl_h = h = info->current_h;
    printf("W, H: (%f, %f)\n", w, h);

	bpp = info->vfmt->BitsPerPixel;

	/*
	 * Now, we want to setup our requested
	 * window attributes for our OpenGL window.
	 * We want *at least* 5 bits of red, green
	 * and blue. We also want at least a 16-bit
	 * depth buffer.
	 *
	 * The last thing we do is request a double
	 * buffered window. '1' turns on double
	 * buffering, '0' turns it off.
	 *
	 * Note that we do not use SDL_DOUBLEBUF in
	 * the flags to SDL_SetVideoMode. That does
	 * not affect the GL attribute state, only
	 * the standard 2D blitting setup.
	 */
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 5);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	/* flags = SDL_OPENGL | SDL_FULLSCREEN; */
	flags = SDL_OPENGL | SDL_RESIZABLE | SDL_DOUBLEBUF;
	/* flags = SDL_OPENGL; */
    surface = SDL_SetVideoMode(w, h, bpp, flags);
	if (!surface) {
		/*
		 * This could happen for a variety of reasons,
		 * including DISPLAY not being set, the specified
		 * resolution not being available, etc.
		 */
		fprintf(stderr, "Video mode set failed: %s\n", SDL_GetError());
		quit_tutorial(1);
	}


    SDL_WM_SetCaption("least", "least");

	return 0;
}

static void process_events(void)
{
	/* Our SDL event placeholder. */
	SDL_Event event;

    /* Only poll + sleep if we are autoscrolling or doing
     * something else that is interactive */
    if ((autoscroll) && autoscroll_var) {
        if (!SDL_PollEvent(&event)) {
            /* If we add a sleep, the scrolling won't be super smooth.
             * Regardless, I think we need to find something to make sure we
             * don't eat 100% cpu just checking for events.
             *
             * I found that 10ms is not a bad wait. Theoretically we want to
             * wait 1000ms / fps (usually 60) -> 16ms.
             * */
            usleep(16000);

            if (autoscroll)
                scroll -= autoscroll_var;

            redraw = 1;
        }
    } else {
        SDL_WaitEvent(&event);
    }

    switch (event.type) {
    case SDL_KEYDOWN:
        /* Handle key presses. */
        handle_key_down(&event.key.keysym);
        break;
    case SDL_QUIT:
        /* Handle quit requests (like Ctrl-c). */
        quit_tutorial(0);
        break;
    case SDL_VIDEORESIZE:
        handle_resize(event.resize);
        break;
    case SDL_VIDEOEXPOSE:
        redraw = 1;
        break;
    }

}

static void draw_screen(void)
{
    unsigned int i;
    int ww, hh;
    int pow2_ww, pow2_hh;

    /* View dimensions of pages */
    float vw, vh;
    /* static float vloot = 0.f; */

    ww = imw;
    hh = imh;

	glEnable(GL_TEXTURE_2D);

    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();

    /* Setup texture matrix to certain scale whenever pow2 is required */
    if (power_of_two) {
        RPOW2(pow2_ww, ww);
        RPOW2(pow2_hh, hh);
        glScalef(ww / (float)pow2_ww, hh / (float)pow2_hh, 1.0f);
    }

	glClearColor(0.5f, 0.5f, 0.5f, 0.0f);
	glViewport(0, 0, (int)w, (int)gl_h);
    /* glViewport(0, 0, 400, 400); */
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, (int)w, (int)gl_h, 0, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

    /*
    vloot += 0.1;
    glRotatef(vloot, 0.f, 0.f, 1.0f);
    */

    /* Compute pageview dimensions */
    vw = w;
    vh = (vw / ww) * hh;

    /* Scale based scroll */
    glTranslatef(0.f, scroll * (vh / imh) , 0.f);

    for(i = 0; i < pagec; i++) {
        /* printf("Page: %d, size: (%f, %f)\n", i, imw, imh); */
        glColor3f(1.0, 1.0, 1.0);

        /* printf("Binding texture: %d\n", pages[i]); */
        glBindTexture(GL_TEXTURE_2D, pages[i]);
        /* printf("OpenGL error: %s\n", gluErrorString(glGetError())); */
        /* Send our triangle data to the pipeline. */
        glBegin(GL_QUADS);

        /* Bottom-left vertex (corner) */
        glTexCoord2i(0, 0);
        glVertex3f(0.f, 0.f, 0.0f);

        /* Bottom-right vertex (corner) */
        glTexCoord2i(1, 0);
        glVertex3f(vw, 0.f, 0.f);

        /* Top-right vertex (corner) */
        glTexCoord2i(1, 1);
        glVertex3f(vw, vh, 0.f);

        /* Top-left vertex (corner) */
        glTexCoord2i(0, 1);
        glVertex3f(0.f, vh, 0.f);

        glEnd();
        /*
        if (i % 2 == 0)
            glTranslatef(ww + 20, 0.f, 0.f);
        else {
            glTranslatef(-ww -20, hh + 20, 0.f);
        }
        */
        glTranslatef(0.0f, vh + (vh / imh) * 20, 0.f);
    }

	/*
	 * Swap the buffers. This this tells the driver to
	 * render the next frame from the contents of the
	 * back-buffer, and to set all rendering operations
	 * to occur on what was the front-buffer.
	 *
	 * Double buffering prevents nasty visual tearing
	 * from the application drawing on areas of the
	 * screen that are being updated at the same time.
	 */
	SDL_GL_SwapBuffers();
}

int main (int argc, char **argv) {
    fz_context *context;

    context = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!context)
        fprintf(stderr, "Failed to create context\n");

    if (argc == 2) {
        /* Initialize OpenGL window */
        setup_sdl();

        /*
         * At this point, we should have a properly setup
         * double-buffered window for use with OpenGL.
         */
        setup_opengl(w, h);

        /* Check for non-power-of-two support */
        /* printf("Extensions are: %s\n", glGetString(GL_EXTENSIONS)); */
        if (strstr((const char *)glGetString(GL_EXTENSIONS),
            "GL_ARB_texture_non_power_of_two")) {
            puts("Machine supports NPOT textures.");
            power_of_two = 0;
        } else {
            puts("Machine supports POT textures only.");
            power_of_two = 1;
        }

        power_of_two |= force_power_of_two;

        /* Load textures from PDF file */
        open_pdf(context, argv[1]);

        /*
         * Now we want to begin our normal app process--
         * an event loop with a lot of redrawing.
         */

        while (1) {
            /* Process incoming events. */
            process_events();

            if (redraw) {
                /*
                glDeleteTextures(pagec, pages);
                open_pdf(context, argv[1]);
                */
                redraw = 0;
                draw_screen();
            }
        }

    }


    fz_free_context(context);

    return 0;
}
