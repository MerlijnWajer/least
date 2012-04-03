#include <SDL/SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>

#include "mupdf/fitz/fitz.h"
#include "mupdf/pdf/mupdf.h"

static float w = 800.0f;
static float h = 600.0f;
float imw, imh;
GLuint *pages;
unsigned int pagec;

static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type);
static int page_to_texture(fz_context *ctx, fz_document *doc, int pagenum);
static void draw_screen(void);

float scroll = 0.0f;

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
    int res;

    res = 72.0f * 1.0;

    ctm = fz_scale(res / 72.0f, res / 72.0f);
    res = (int)(72 * 1.0f);
    ctm = fz_identity;


    printf("Rendering page %d\n", pagenum);
    page = fz_load_page(doc, pagenum);

    bounds = fz_bound_page(doc, page);
    bounds.x1 *= res / 72.0f;
    bounds.y1 *= res / 72.0f;
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

    /*
    s = malloc(sizeof(char) * 20);
    sprintf(s, "/tmp/test%d.png", i);

    printf("Saving page %d to %s\n", i, s);
    fz_write_png(context, image, s, 0);
    free(s);
    */

    fz_drop_pixmap(context, image);

    fz_free_device(dev);

    fz_free_page(doc, page);

    return pages[pagenum];
}

/*
static int reload_texture(fz_context *context, fz_document *doc, int pagenum) {


    return pages[pagenum];
}
*/

static int pixmap_to_texture(void *pixmap, int width, int height, int format, int type)
{
    unsigned int texname;

    format = 42;
    type = 31337;

    glGenTextures(1, &texname);

    /* Bind the texture object */
    glBindTexture(GL_TEXTURE_2D, texname);

    /* Set the texture's stretching properties */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
            GL_LINEAR);

    /* Edit the texture object's image data using the information SDL_Surface gives us */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width,
             height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
             pixmap);

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
        scroll -= 42.;
        break;

    case SDLK_UP:
        scroll += 42.;
        break;

    case SDLK_PAGEDOWN:
        scroll -= imh + 20;
        break;

    case SDLK_PAGEUP:
        scroll += imh + 20;
        break;

	default:
		break;
	}
}

static void handle_resize(SDL_ResizeEvent e) {
    /* printf("Resized to (%d, %d)\n", e.w, e.h); */
    w = e.w;
    h = e.h;

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
	/* Dimensions of our window. */
	int width = 0;
	int height = 0;
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

	/*
	 * Set our width/height to 640/480 (you would
	 * of course let the user decide this in a normal
	 * app). We get the bpp we will request from
	 * the display. On X11, VidMode can't change
	 * resolution, so this is probably being overly
	 * safe. Under Win32, ChangeDisplaySettings
	 * can change the bpp.
	 */
	width = (int)w;
	height = (int)h;
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
	/* flags = SDL_OPENGL | SDL_RESIZABLE; */
	flags = SDL_OPENGL;

	if (SDL_SetVideoMode(width, height, bpp, flags) == 0) {
		/* 
		 * This could happen for a variety of reasons,
		 * including DISPLAY not being set, the specified
		 * resolution not being available, etc.
		 */
		fprintf(stderr, "Video mode set failed: %s\n", SDL_GetError());
		quit_tutorial(1);
	}


	SDL_SetVideoMode(width, height, bpp, flags);

	/*
	 * EXERCISE:
	 * Record timings using SDL_GetTicks() and
	 * and print out frames per second at program
	 * end.
	 */

	/* Never reached. */
	return 0;
}

static void process_events(void)
{
	/* Our SDL event placeholder. */
	SDL_Event event;

	/* Grab all the events off the queue. */
	while (SDL_PollEvent(&event)) {

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
		}

	}

}

static void draw_screen(void)
{
    unsigned int i;
    /* static float vloot = 0.f; */

	glEnable(GL_TEXTURE_2D);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glViewport(0, 0, (int)w, (int)h);
	glClear(GL_COLOR_BUFFER_BIT);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0f, (int)w, (int)h, 0.0f, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

    /*
    vloot += 0.1;
    glRotatef(vloot, 0.f, 0.f, 1.0f);
    */
    glTranslatef(0.f, scroll, 0.f);

    for(i = 0; i < pagec; i++) {
        printf("Page: %d, size: (%f, %f)\n", i, imw, imh);
        glBindTexture(GL_TEXTURE_2D, pages[i]);
        /* Send our triangle data to the pipeline. */
        glBegin(GL_QUADS);

        /* Bottom-left vertex (corner) */
        glTexCoord2i(0, 0);
        glVertex3f(0.f, 0.f, 0.0f);

        /* Bottom-right vertex (corner) */
        glTexCoord2i(1, 0);
        glVertex3f(imw, 0.f, 0.f);

        /* Top-right vertex (corner) */
        glTexCoord2i(1, 1);
        glVertex3f(imw, imh, 0.f);

        /* Top-left vertex (corner) */
        glTexCoord2i(0, 1);
        glVertex3f(0.f, imh, 0.f);

        glEnd();
        /*
        if (i % 2 == 0)
            glTranslatef(imw + 20, 0.f, 0.f);
        else {
            glTranslatef(-imw -20, imh + 20, 0.f);
        }
        */
        glTranslatef(0.0f, imh + 20, 0.f);
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
    int t, fc;

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

        /* Load textures from PDF file */
        open_pdf(context, argv[1]);

        /*
         * Now we want to begin our normal app process--
         * an event loop with a lot of redrawing.
         */

        while (1) {
            if (SDL_GetTicks() - t > 1000) {
                /* printf("Drew %d FPS in 1 second\n", fc); */
                t = SDL_GetTicks();
                fc = 0;
            }
            /* Process incoming events. */
            process_events();
            /* Draw the screen. */
            draw_screen();
            fc++;
        }

    }


    fz_free_context(context);

    return 0;
}
