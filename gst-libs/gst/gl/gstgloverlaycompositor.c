/*
 * GStreamer
 * Copyright (C) 2015 Lubosz Sarnecki <lubosz.sarnecki@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>

#include "gl.h"
#include "gstgloverlaycompositor.h"

GST_DEBUG_CATEGORY_STATIC (gst_gl_overlay_compositor_debug);
#define GST_CAT_DEFAULT gst_gl_overlay_compositor_debug

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_overlay_compositor_debug, \
      "gloverlaycompositor", 0, "overlaycompositor");

G_DEFINE_TYPE_WITH_CODE (GstGLOverlayCompositor, gst_gl_overlay_compositor,
    GST_TYPE_OBJECT, DEBUG_INIT);

static void gst_gl_overlay_compositor_finalize (GObject * object);
static gboolean _is_rectangle_in_overlays (GList * overlays,
    GstVideoOverlayRectangle * rectangle);
static gboolean _is_overlay_in_rectangles (GstVideoOverlayComposition *
    composition, GstGLCompositionOverlay * overlay);

static void
gst_gl_overlay_compositor_class_init (GstGLOverlayCompositorClass * klass)
{
  G_OBJECT_CLASS (klass)->finalize = gst_gl_overlay_compositor_finalize;
}

static void
gst_gl_overlay_compositor_init (GstGLOverlayCompositor * compositor)
{
}

GstGLOverlayCompositor *
gst_gl_overlay_compositor_new (GstGLContext * context)
{
  GstGLOverlayCompositor *compositor =
      g_object_new (GST_TYPE_GL_OVERLAY_COMPOSITOR, NULL);

  compositor->context = gst_object_ref (context);

  GST_DEBUG_OBJECT (compositor, "Created new GstGLOverlayCompositor");

  return compositor;
}

static void
gst_gl_overlay_compositor_finalize (GObject * object)
{
  GstGLOverlayCompositor *compositor;

  compositor = GST_GL_OVERLAY_COMPOSITOR (object);

  gst_gl_overlay_compositor_free_overlays (compositor);

  if (compositor->context)
    gst_object_unref (compositor->context);

  G_OBJECT_CLASS (gst_gl_overlay_compositor_parent_class)->finalize (object);
}

static gboolean
_is_rectangle_in_overlays (GList * overlays,
    GstVideoOverlayRectangle * rectangle)
{
  for (GList * l = overlays; l != NULL; l = l->next) {
    GstGLCompositionOverlay *overlay = (GstGLCompositionOverlay *) l->data;
    if (overlay->rectangle == rectangle)
      return TRUE;
  }
  return FALSE;
}

static gboolean
_is_overlay_in_rectangles (GstVideoOverlayComposition * composition,
    GstGLCompositionOverlay * overlay)
{
  for (guint i = 0;
      i < gst_video_overlay_composition_n_rectangles (composition); i++) {
    GstVideoOverlayRectangle *rectangle =
        gst_video_overlay_composition_get_rectangle (composition, i);
    if (overlay->rectangle == rectangle)
      return TRUE;
  }
  return FALSE;
}


void
gst_gl_overlay_compositor_free_overlays (GstGLOverlayCompositor * compositor)
{
  GList *l = compositor->overlays;
  while (l != NULL) {
    GList *next = l->next;
    GstGLCompositionOverlay *overlay = (GstGLCompositionOverlay *) l->data;
    compositor->overlays = g_list_delete_link (compositor->overlays, l);
    gst_object_unref (overlay);
    l = next;
  }
  g_list_free (compositor->overlays);
  compositor->overlays = NULL;
}

void
gst_gl_overlay_compositor_upload_overlays (GstGLOverlayCompositor * compositor,
    GstBuffer * buf, GLint position_attrib, GLint texcoord_attrib,
    guint window_width, guint window_height)
{
  GstVideoOverlayCompositionMeta *composition_meta;

  if (compositor->last_window_width != window_width ||
      compositor->last_window_height != window_height) {
    gst_gl_overlay_compositor_free_overlays (compositor);
    compositor->last_window_width = window_width;
    compositor->last_window_height = window_height;
    GST_DEBUG ("window size changed, freeing overlays");
  }

  composition_meta = gst_buffer_get_video_overlay_composition_meta (buf);
  if (composition_meta) {
    GstVideoOverlayComposition *composition = NULL;
    guint num_overlays;
    GList *l = compositor->overlays;

    GST_DEBUG ("GstVideoOverlayCompositionMeta found.");

    composition = composition_meta->overlay;
    num_overlays = gst_video_overlay_composition_n_rectangles (composition);

    /* add new overlays to list */
    for (guint i = 0; i < num_overlays; i++) {
      GstVideoOverlayRectangle *rectangle =
          gst_video_overlay_composition_get_rectangle (composition, i);

      if (!_is_rectangle_in_overlays (compositor->overlays, rectangle)) {
        GstGLCompositionOverlay *overlay =
            gst_gl_composition_overlay_new (compositor->context, rectangle,
            position_attrib, texcoord_attrib);

        gst_gl_composition_overlay_upload (overlay, buf,
            window_width, window_height);
        compositor->overlays = g_list_append (compositor->overlays, overlay);
      }
    }

    /* remove old overlays from list */
    while (l != NULL) {
      GList *next = l->next;
      GstGLCompositionOverlay *overlay = (GstGLCompositionOverlay *) l->data;
      if (!_is_overlay_in_rectangles (composition, overlay)) {
        compositor->overlays = g_list_delete_link (compositor->overlays, l);
        gst_object_unref (overlay);
      }
      l = next;
    }
  } else {
    gst_gl_overlay_compositor_free_overlays (compositor);
  }
}

void
gst_gl_overlay_compositor_draw_overlays (GstGLOverlayCompositor * compositor,
    GstGLShader * shader)
{
  const GstGLFuncs *gl = compositor->context->gl_vtable;
  if (compositor->overlays != NULL) {
    glEnable (GL_BLEND);
    glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (GList * l = compositor->overlays; l != NULL; l = l->next) {
      GstGLCompositionOverlay *overlay = (GstGLCompositionOverlay *) l->data;
      gst_gl_composition_overlay_draw (overlay, shader);
    }
    gl->BindTexture (GL_TEXTURE_2D, 0);
  }
}

GstCaps *
gst_gl_overlay_compositor_add_caps (GstCaps * caps)
{
  GstCaps *composition_caps;

  composition_caps = gst_caps_copy (caps);

  for (int i = 0; i < gst_caps_get_size (composition_caps); i++) {
    GstCapsFeatures *f = gst_caps_get_features (composition_caps, i);
    gst_caps_features_add (f,
        GST_CAPS_FEATURE_META_GST_VIDEO_OVERLAY_COMPOSITION);
  }

  caps = gst_caps_merge (composition_caps, caps);

  return caps;
}
