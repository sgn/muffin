/*
 * shaped texture
 *
 * An actor to draw a texture clipped to a list of rectangles
 *
 * Authored By Neil Roberts  <neil@linux.intel.com>
 *
 * Copyright (C) 2008 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street - Suite 500, Boston, MA
 * 02110-1335, USA.
 */

/**
 * SECTION:meta-shaped-texture
 * @title: MetaShapedTexture
 * @short_description: An actor to draw a masked texture.
 */

#include <config.h>

#include <meta/meta-shaped-texture.h>
#include "meta-texture-tower.h"
#include "meta-texture-rectangle.h"
#include "cogl-utils.h"

#include <clutter/clutter.h>
#include <cogl/cogl.h>
#include <cogl/winsys/cogl-texture-pixmap-x11.h>
#include <gdk/gdk.h> /* for gdk_rectangle_intersect() */
#include <string.h>

/* MAX_MIPMAPPING_FPS needs to be as small as possible for the best GPU
 * performance, but higher than the refresh rate of commonly slow updating
 * windows like top or a blinking cursor, so that such windows do get
 * mipmapped.
 */
#define MAX_MIPMAPPING_FPS 5
#define MIN_MIPMAP_AGE_USEC (G_USEC_PER_SEC / MAX_MIPMAPPING_FPS)

/* MIN_FAST_UPDATES_BEFORE_UNMIPMAP allows windows to update themselves
 * occasionally without causing mipmapping to be disabled, so long as such
 * an update takes fewer update_area calls than:
 */
#define MIN_FAST_UPDATES_BEFORE_UNMIPMAP 20

static void meta_shaped_texture_dispose  (GObject    *object);

static void meta_shaped_texture_paint (ClutterActor       *actor);
static void meta_shaped_texture_pick  (ClutterActor       *actor,
				       const ClutterColor *color);

static void meta_shaped_texture_get_preferred_width (ClutterActor *self,
                                                     gfloat        for_height,
                                                     gfloat       *min_width_p,
                                                     gfloat       *natural_width_p);

static void meta_shaped_texture_get_preferred_height (ClutterActor *self,
                                                      gfloat        for_width,
                                                      gfloat       *min_height_p,
                                                      gfloat       *natural_height_p);

static void meta_shaped_texture_dirty_mask (MetaShapedTexture *stex);

static gboolean meta_shaped_texture_get_paint_volume (ClutterActor *self, ClutterPaintVolume *volume);

G_DEFINE_TYPE (MetaShapedTexture, meta_shaped_texture,
               CLUTTER_TYPE_ACTOR);

#define META_SHAPED_TEXTURE_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), META_TYPE_SHAPED_TEXTURE, \
                                MetaShapedTexturePrivate))

struct _MetaShapedTexturePrivate
{
  MetaTextureTower *paint_tower;
  Pixmap pixmap;
  CoglTexture *texture;
  CoglTexture *mask_texture;
  CoglPipeline *pipeline;
  CoglPipeline *pipeline_unshaped;

  cairo_region_t *clip_region;
  cairo_region_t *unobscured_region;
  cairo_region_t *shape_region;

  cairo_region_t *overlay_region;
  cairo_path_t *overlay_path;

  guint tex_width, tex_height;
  guint mask_width, mask_height;

  gint64 prev_invalidation, last_invalidation;
  guint fast_updates;
  guint remipmap_timeout_id;
  gint64 earliest_remipmap;

  guint create_mipmaps : 1;
};

static void
meta_shaped_texture_class_init (MetaShapedTextureClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  ClutterActorClass *actor_class = (ClutterActorClass *) klass;

  gobject_class->dispose = meta_shaped_texture_dispose;

  actor_class->get_preferred_width = meta_shaped_texture_get_preferred_width;
  actor_class->get_preferred_height = meta_shaped_texture_get_preferred_height;
  actor_class->paint = meta_shaped_texture_paint;
  actor_class->pick = meta_shaped_texture_pick;
  actor_class->get_paint_volume = meta_shaped_texture_get_paint_volume;

  g_type_class_add_private (klass, sizeof (MetaShapedTexturePrivate));
}

static void
meta_shaped_texture_init (MetaShapedTexture *self)
{
  MetaShapedTexturePrivate *priv;

  priv = self->priv = META_SHAPED_TEXTURE_GET_PRIVATE (self);

  priv->shape_region = NULL;
  priv->overlay_path = NULL;
  priv->overlay_region = NULL;
  priv->paint_tower = meta_texture_tower_new ();
  priv->texture = NULL;
  priv->mask_texture = NULL;
  priv->create_mipmaps = TRUE;
}

static void
meta_shaped_texture_dispose (GObject *object)
{
  MetaShapedTexture *self = (MetaShapedTexture *) object;
  MetaShapedTexturePrivate *priv = self->priv;

  if (priv->remipmap_timeout_id)
    {
      g_source_remove (priv->remipmap_timeout_id);
      priv->remipmap_timeout_id = 0;
    }

  if (priv->paint_tower)
    meta_texture_tower_free (priv->paint_tower);
  priv->paint_tower = NULL;

  meta_shaped_texture_dirty_mask (self);

  if (priv->pipeline != NULL)
    {
      cogl_object_unref (priv->pipeline);
      priv->pipeline = NULL;
    }
  if (priv->pipeline_unshaped != NULL)
    {
      cogl_object_unref (priv->pipeline_unshaped);
      priv->pipeline_unshaped = NULL;
    }
  if (priv->texture != NULL)
    {
      cogl_object_unref (priv->texture);
      priv->texture = NULL;
    }

  meta_shaped_texture_set_shape_region (self, NULL);
  meta_shaped_texture_set_clip_region (self, NULL);
  meta_shaped_texture_set_overlay_path (self, NULL, NULL);

  G_OBJECT_CLASS (meta_shaped_texture_parent_class)->dispose (object);
}

static void
meta_shaped_texture_dirty_mask (MetaShapedTexture *stex)
{
  MetaShapedTexturePrivate *priv = stex->priv;

  if (priv->mask_texture != NULL)
    {
      cogl_object_unref (priv->mask_texture);
      priv->mask_texture = NULL;
    }

  if (priv->pipeline != NULL)
    cogl_pipeline_set_layer_texture (priv->pipeline, 1, NULL);
}

static void
install_overlay_path (MetaShapedTexture *stex,
                      guchar            *mask_data,
                      int                tex_width,
                      int                tex_height,
                      int                stride)
{
  MetaShapedTexturePrivate *priv = stex->priv;
  int i, n_rects;
  cairo_t *cr;
  cairo_rectangle_int_t rect;
  cairo_surface_t *surface;

  if (priv->overlay_region == NULL)
    return;

  surface = cairo_image_surface_create_for_data (mask_data,
                                                 CAIRO_FORMAT_A8,
                                                 tex_width,
                                                 tex_height,
                                                 stride);

  cr = cairo_create (surface);
  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);

  n_rects = cairo_region_num_rectangles (priv->overlay_region);
  for (i = 0; i < n_rects; i++)
    {
      cairo_region_get_rectangle (priv->overlay_region, i, &rect);
      cairo_rectangle (cr, rect.x, rect.y, rect.width, rect.height);
    }

  cairo_fill_preserve (cr);
  if (priv->overlay_path == NULL)
    {
      /* If we have an overlay region but not an overlay path, then we
       * just need to clear the rectangles in the overlay region. */
      goto out;
    }

  cairo_clip (cr);

  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_rgba (cr, 1, 1, 1, 1);

  cairo_append_path (cr, priv->overlay_path);
  cairo_fill (cr);

 out:
  cairo_destroy (cr);
  cairo_surface_destroy (surface);
}

static void
meta_shaped_texture_ensure_mask (MetaShapedTexture *stex)
{
  MetaShapedTexturePrivate *priv = stex->priv;
  CoglTexture *paint_tex;
  guint tex_width, tex_height;

  paint_tex = priv->texture;

  if (paint_tex == NULL)
    return;

  tex_width = cogl_texture_get_width (paint_tex);
  tex_height = cogl_texture_get_height (paint_tex);

  /* If the mask texture we have was created for a different size then
     recreate it */
  if (priv->mask_texture != NULL
      && (priv->mask_width != tex_width || priv->mask_height != tex_height))
    meta_shaped_texture_dirty_mask (stex);

  /* If we don't have a mask texture yet then create one */
  if (priv->mask_texture == NULL)
    {
      guchar *mask_data;
      int i;
      int n_rects;
      int stride;

      /* If we have no shape region and no (or an empty) overlay region, we
       * don't need to create a full mask texture, so quit early. */
      if (priv->shape_region == NULL &&
          (priv->overlay_region == NULL ||
           cairo_region_num_rectangles (priv->overlay_region) == 0))
        {
          return;
        }

      stride = cairo_format_stride_for_width (CAIRO_FORMAT_A8, tex_width);

      /* Create data for an empty image */
      mask_data = g_malloc0 (stride * tex_height);

      n_rects = cairo_region_num_rectangles (priv->shape_region);

      /* Fill in each rectangle. */
      for (i = 0; i < n_rects; i ++)
        {
          cairo_rectangle_int_t rect;
          cairo_region_get_rectangle (priv->shape_region, i, &rect);

          gint x1 = rect.x, x2 = x1 + rect.width;
          gint y1 = rect.y, y2 = y1 + rect.height;
          guchar *p;

          /* Clip the rectangle to the size of the texture */
          x1 = CLAMP (x1, 0, (gint) tex_width - 1);
          x2 = CLAMP (x2, x1, (gint) tex_width);
          y1 = CLAMP (y1, 0, (gint) tex_height - 1);
          y2 = CLAMP (y2, y1, (gint) tex_height);

          /* Fill the rectangle */
          for (p = mask_data + y1 * stride + x1;
               y1 < y2;
               y1++, p += stride)
            memset (p, 255, x2 - x1);
        }

      install_overlay_path (stex, mask_data, tex_width, tex_height, stride);

      if (meta_texture_rectangle_check (paint_tex))
        priv->mask_texture = meta_texture_rectangle_new (tex_width, tex_height,
                                                         COGL_PIXEL_FORMAT_A_8,
                                                         stride,
                                                         mask_data);
      else
        priv->mask_texture = meta_cogl_texture_new_from_data_wrapper (tex_width, tex_height,
                                                                      COGL_TEXTURE_NONE,
                                                                      COGL_PIXEL_FORMAT_A_8,
                                                                      COGL_PIXEL_FORMAT_ANY,
                                                                      stride,
                                                                      mask_data);

      g_free (mask_data);

      priv->mask_width = tex_width;
      priv->mask_height = tex_height;
    }
}

static gboolean
texture_is_idle_and_not_mipmapped (gpointer user_data)
{
  MetaShapedTexture *stex = META_SHAPED_TEXTURE (user_data);
  MetaShapedTexturePrivate *priv = stex->priv;

  if ((g_get_monotonic_time () - priv->earliest_remipmap) < 0)
    return G_SOURCE_CONTINUE;

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stex));
  priv->remipmap_timeout_id = 0;

  return G_SOURCE_REMOVE;
}

static void
meta_shaped_texture_paint (ClutterActor *actor)
{
  MetaShapedTexture *stex = (MetaShapedTexture *) actor;
  MetaShapedTexturePrivate *priv = stex->priv;
  CoglContext *ctx = clutter_backend_get_cogl_context (clutter_get_default_backend ());
  CoglTexture *paint_tex = NULL;
  guint tex_width, tex_height;
  ClutterActorBox alloc;
  gint64 now = g_get_monotonic_time ();

  CoglPipeline *pipeline_template = NULL;
  CoglPipeline *pipeline_unshaped_template = NULL;
  CoglPipeline *pipeline;

  if (priv->clip_region && cairo_region_is_empty (priv->clip_region))
    return;

  if (!CLUTTER_ACTOR_IS_REALIZED (CLUTTER_ACTOR (stex)))
    clutter_actor_realize (CLUTTER_ACTOR (stex));

  /* The GL EXT_texture_from_pixmap extension does allow for it to be
   * used together with SGIS_generate_mipmap, however this is very
   * rarely supported. Also, even when it is supported there
   * are distinct performance implications from:
   *
   *  - Updating mipmaps that we don't need
   *  - Having to reallocate pixmaps on the server into larger buffers
   *
   * So, we just unconditionally use our mipmap emulation code. If we
   * wanted to use SGIS_generate_mipmap, we'd have to  query COGL to
   * see if it was supported (no API currently), and then if and only
   * if that was the case, set the clutter texture quality to HIGH.
   * Setting the texture quality to high without SGIS_generate_mipmap
   * support for TFP textures will result in fallbacks to XGetImage.
   */
  if (priv->create_mipmaps && priv->last_invalidation)
    {
      gint64 age = now - priv->last_invalidation;

      if (age >= MIN_MIPMAP_AGE_USEC ||
          priv->fast_updates < MIN_FAST_UPDATES_BEFORE_UNMIPMAP)
        paint_tex = meta_texture_tower_get_paint_texture (priv->paint_tower);
    }

  if (paint_tex == NULL)
    {
      paint_tex = COGL_TEXTURE (priv->texture);

      if (paint_tex == NULL)
        return;

      if (priv->create_mipmaps)
        {
          /* Minus 1000 to ensure we don't fail the age test in timeout */
          priv->earliest_remipmap = now + MIN_MIPMAP_AGE_USEC - 1000;

          if (!priv->remipmap_timeout_id)
            priv->remipmap_timeout_id =
              g_timeout_add (MIN_MIPMAP_AGE_USEC / 1000,
                             texture_is_idle_and_not_mipmapped,
                             stex);
        }
    }

  tex_width = priv->tex_width;
  tex_height = priv->tex_height;

  if (tex_width == 0 || tex_height == 0) /* no contents yet */
    return;

  if (priv->shape_region == NULL)
    {
      /* No region means an unclipped shape. Use a single-layer texture. */

      if (priv->pipeline_unshaped == NULL)
        {
          if (G_UNLIKELY (pipeline_unshaped_template == NULL))
            {
              pipeline_unshaped_template = cogl_pipeline_new (ctx);
            }

          priv->pipeline_unshaped = cogl_pipeline_copy (pipeline_unshaped_template);
        }
        pipeline = priv->pipeline_unshaped;
    }
  else
    {
      meta_shaped_texture_ensure_mask (stex);

      if (priv->pipeline == NULL)
	{
	   if (G_UNLIKELY (pipeline_template == NULL))
	    {
	      pipeline_template =  cogl_pipeline_new (ctx);
	      cogl_pipeline_set_layer_combine (pipeline_template, 1,
					   "RGBA = MODULATE (PREVIOUS, TEXTURE[A])",
					   NULL);
	    }
	  priv->pipeline = cogl_pipeline_copy (pipeline_template);
	}
      pipeline = priv->pipeline;

      cogl_pipeline_set_layer_texture (pipeline, 1, priv->mask_texture);
    }

  cogl_pipeline_set_layer_texture (pipeline, 0, paint_tex);

  {
    CoglColor color;
    guchar opacity = clutter_actor_get_paint_opacity (actor);
    cogl_color_set_from_4ub (&color, opacity, opacity, opacity, opacity);
    cogl_pipeline_set_color (pipeline, &color);
  }

  cogl_set_source (pipeline);

  clutter_actor_get_allocation_box (actor, &alloc);

  if (priv->clip_region)
    {
      int n_rects;
      int i;
      cairo_rectangle_int_t tex_rect = { 0, 0, tex_width, tex_height };

      /* Limit to how many separate rectangles we'll draw; beyond this just
       * fall back and draw the whole thing */
#     define MAX_RECTS 16

      n_rects = cairo_region_num_rectangles (priv->clip_region);
      if (n_rects <= MAX_RECTS)
	{
	  float coords[8];
          float x1, y1, x2, y2;

	  for (i = 0; i < n_rects; i++)
	    {
	      cairo_rectangle_int_t rect;

	      cairo_region_get_rectangle (priv->clip_region, i, &rect);

	      if (!gdk_rectangle_intersect (&tex_rect, &rect, &rect))
		continue;

	      x1 = rect.x;
	      y1 = rect.y;
	      x2 = rect.x + rect.width;
	      y2 = rect.y + rect.height;

	      coords[0] = rect.x / (alloc.x2 - alloc.x1);
	      coords[1] = rect.y / (alloc.y2 - alloc.y1);
	      coords[2] = (rect.x + rect.width) / (alloc.x2 - alloc.x1);
	      coords[3] = (rect.y + rect.height) / (alloc.y2 - alloc.y1);

              coords[4] = coords[0];
              coords[5] = coords[1];
              coords[6] = coords[2];
              coords[7] = coords[3];

              cogl_rectangle_with_multitexture_coords (x1, y1, x2, y2,
                                                       &coords[0], 8);
            }

	  return;
	}
    }

  cogl_rectangle (0, 0,
		  alloc.x2 - alloc.x1,
		  alloc.y2 - alloc.y1);
}

static void
meta_shaped_texture_pick (ClutterActor       *actor,
			  const ClutterColor *color)
{
  MetaShapedTexture *stex = (MetaShapedTexture *) actor;
  MetaShapedTexturePrivate *priv = stex->priv;

  /* If there is no region then use the regular pick */
  if (priv->shape_region == NULL)
    CLUTTER_ACTOR_CLASS (meta_shaped_texture_parent_class)
      ->pick (actor, color);
  else if (clutter_actor_should_pick_paint (actor))
    {
      CoglTexture *paint_tex;
      ClutterActorBox alloc;
      guint tex_width, tex_height;

      paint_tex = priv->texture;

      if (paint_tex == NULL)
        return;

      tex_width = cogl_texture_get_width (paint_tex);
      tex_height = cogl_texture_get_height (paint_tex);

      if (tex_width == 0 || tex_height == 0) /* no contents yet */
        return;

      meta_shaped_texture_ensure_mask (stex);

      cogl_set_source_color4ub (color->red, color->green, color->blue,
                                 color->alpha);

      clutter_actor_get_allocation_box (actor, &alloc);

      /* Paint the mask rectangle in the given color */
      cogl_set_source_texture (priv->mask_texture);
      cogl_rectangle_with_texture_coords (0, 0,
                                          alloc.x2 - alloc.x1,
                                          alloc.y2 - alloc.y1,
                                          0, 0, 1, 1);
    }
}

static void
meta_shaped_texture_get_preferred_width (ClutterActor *self,
                                         gfloat        for_height,
                                         gfloat       *min_width_p,
                                         gfloat       *natural_width_p)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (self));

  priv = META_SHAPED_TEXTURE (self)->priv;

  if (min_width_p)
    *min_width_p = 0;

  if (natural_width_p)
    *natural_width_p = priv->tex_width;
}

static void
meta_shaped_texture_get_preferred_height (ClutterActor *self,
                                          gfloat        for_width,
                                          gfloat       *min_height_p,
                                          gfloat       *natural_height_p)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (self));

  priv = META_SHAPED_TEXTURE (self)->priv;

  if (min_height_p)
    *min_height_p = 0;

  if (natural_height_p)
    *natural_height_p = priv->tex_height;
}

static gboolean
meta_shaped_texture_get_paint_volume (ClutterActor *self,
                                      ClutterPaintVolume *volume)
{
  return clutter_paint_volume_set_from_allocation (volume, self);
}

ClutterActor *
meta_shaped_texture_new (void)
{
  ClutterActor *self = g_object_new (META_TYPE_SHAPED_TEXTURE, NULL);

  return self;
}

void
meta_shaped_texture_set_create_mipmaps (MetaShapedTexture *stex,
					gboolean           create_mipmaps)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  create_mipmaps = create_mipmaps != FALSE;

  if (create_mipmaps != priv->create_mipmaps)
    {
      CoglTexture *base_texture;
      priv->create_mipmaps = create_mipmaps;
      base_texture = create_mipmaps ?
        priv->texture : NULL;
      meta_texture_tower_set_base_texture (priv->paint_tower, base_texture);
    }
}

void
meta_shaped_texture_set_shape_region (MetaShapedTexture *stex,
                                      cairo_region_t    *region)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  if (priv->shape_region != NULL)
    {
      cairo_region_destroy (priv->shape_region);
      priv->shape_region = NULL;
    }

  if (region != NULL)
    {
      cairo_region_reference (region);
      priv->shape_region = region;
    }

  meta_shaped_texture_dirty_mask (stex);
  clutter_actor_queue_redraw (CLUTTER_ACTOR (stex));
}

static cairo_region_t *
effective_unobscured_region (MetaShapedTexture *self)
{
  MetaShapedTexturePrivate *priv = self->priv;

  return clutter_actor_has_mapped_clones (CLUTTER_ACTOR (self)) ? NULL : priv->unobscured_region;
}

gboolean
meta_shaped_texture_is_obscured (MetaShapedTexture *self)
{
  cairo_region_t *unobscured_region = effective_unobscured_region (self);

  if (unobscured_region)
    return cairo_region_is_empty (unobscured_region);
  else
    return FALSE;
}

/**
 * meta_shaped_texture_update_area:
 * @stex: #MetaShapedTexture
 * @x: the x coordinate of the damaged area
 * @y: the y coordinate of the damaged area
 * @width: the width of the damaged area
 * @height: the height of the damaged area
 * @unobscured_region: The unobscured region of the window or %NULL if
 * there is no valid one (like when the actor is transformed or
 * has a mapped clone)
 *
 * Repairs the damaged area indicated by @x, @y, @width and @height
 * and queues a redraw for the intersection @visibible_region and
 * the damage area. If @visibible_region is %NULL a redraw will always
 * get queued.
 *
 * Return value: Whether a redraw have been queued or not
 */
gboolean
meta_shaped_texture_update_area (MetaShapedTexture *stex,
				 int                x,
				 int                y,
				 int                width,
				 int                height,
				 cairo_region_t    *unobscured_region)
{
  MetaShapedTexturePrivate *priv;
  const cairo_rectangle_int_t clip = { x, y, width, height };

  priv = stex->priv;

  if (priv->texture == NULL)
    return FALSE;

  cogl_texture_pixmap_x11_update_area (COGL_TEXTURE_PIXMAP_X11 (priv->texture),
                                       x, y, width, height);

  meta_texture_tower_update_area (priv->paint_tower, x, y, width, height);

  priv->prev_invalidation = priv->last_invalidation;
  priv->last_invalidation = g_get_monotonic_time ();

  if (priv->prev_invalidation)
    {
      gint64 interval = priv->last_invalidation - priv->prev_invalidation;
      gboolean fast_update = interval < MIN_MIPMAP_AGE_USEC;

      if (!fast_update)
        priv->fast_updates = 0;
      else if (priv->fast_updates < MIN_FAST_UPDATES_BEFORE_UNMIPMAP)
        priv->fast_updates++;
    }

  if (unobscured_region)
    {
      cairo_region_t *intersection;

      if (cairo_region_is_empty (unobscured_region))
        return FALSE;

      intersection = cairo_region_copy (unobscured_region);
      cairo_region_intersect_rectangle (intersection, &clip);

      if (!cairo_region_is_empty (intersection))
        {
          cairo_rectangle_int_t damage_rect;
          cairo_region_get_extents (intersection, &damage_rect);
          clutter_actor_queue_redraw_with_clip (CLUTTER_ACTOR (stex), &damage_rect);
          cairo_region_destroy (intersection);

          return TRUE;
        }

      cairo_region_destroy (intersection);

      return FALSE;
    }

  clutter_actor_queue_redraw_with_clip (CLUTTER_ACTOR (stex), &clip);

  return TRUE;
}

static void
set_cogl_texture (MetaShapedTexture *stex,
                  CoglTexture     *cogl_tex)
{
  MetaShapedTexturePrivate *priv;
  guint width, height;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  if (priv->texture != NULL)
    cogl_object_unref (priv->texture);

  priv->texture = cogl_tex;

  if (priv->pipeline != NULL)
    cogl_pipeline_set_layer_texture (priv->pipeline, 0, cogl_tex);

  if (priv->pipeline_unshaped != NULL)
    cogl_pipeline_set_layer_texture (priv->pipeline_unshaped, 0, cogl_tex);

  if (cogl_tex != NULL)
    {
      width = cogl_texture_get_width (cogl_tex);
      height = cogl_texture_get_height (cogl_tex);

      if (width != priv->tex_width ||
          height != priv->tex_height)
        {
          priv->tex_width = width;
          priv->tex_height = height;

          clutter_actor_queue_relayout (CLUTTER_ACTOR (stex));
        }
    }
  else
    {
      /* size changed to 0 going to an invalid handle */
      priv->tex_width = 0;
      priv->tex_height = 0;
      clutter_actor_queue_relayout (CLUTTER_ACTOR (stex));
    }

  clutter_actor_queue_redraw (CLUTTER_ACTOR (stex));
}

/**
 * meta_shaped_texture_set_pixmap:
 * @stex: The #MetaShapedTexture
 * @pixmap: The pixmap you want the stex to assume
 */
void
meta_shaped_texture_set_pixmap (MetaShapedTexture *stex,
                                Pixmap             pixmap)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  if (priv->pixmap == pixmap)
    return;

  priv->pixmap = pixmap;

  if (pixmap != None)
    {
      CoglContext *ctx =
        clutter_backend_get_cogl_context (clutter_get_default_backend ());
      set_cogl_texture (stex, COGL_TEXTURE (cogl_texture_pixmap_x11_new (ctx, pixmap, FALSE, NULL)));
    }
  else
    set_cogl_texture (stex, NULL);

  if (priv->create_mipmaps)
    meta_texture_tower_set_base_texture (priv->paint_tower, priv->texture);
}

/**
 * meta_shaped_texture_get_texture:
 * @stex: The #MetaShapedTexture
 *
 * Returns: (transfer none): the unshaped texture
 */
CoglTexture *
meta_shaped_texture_get_texture (MetaShapedTexture *stex)
{
  g_return_val_if_fail (META_IS_SHAPED_TEXTURE (stex), NULL);
  return stex->priv->texture;
}

/**
 * meta_shaped_texture_set_overlay_path:
 * @stex: a #MetaShapedTexture
 * @overlay_region: A region containing the parts of the mask to overlay.
 *   All rectangles in this region are wiped clear to full transparency,
 *   and the overlay path is clipped to this region.
 * @overlay_path: (transfer full): This path will be painted onto the mask
 *   texture with a fully opaque source. Due to the lack of refcounting
 *   in #cairo_path_t, ownership of the path is assumed.
 */
void
meta_shaped_texture_set_overlay_path (MetaShapedTexture *stex,
                                      cairo_region_t    *overlay_region,
                                      cairo_path_t      *overlay_path)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  if (priv->overlay_region != NULL)
    {
      cairo_region_destroy (priv->overlay_region);
      priv->overlay_region = NULL;
    }

  if (priv->overlay_path != NULL)
    {
      cairo_path_destroy (priv->overlay_path);
      priv->overlay_path = NULL;
    }

  cairo_region_reference (overlay_region);
  priv->overlay_region = overlay_region;

  /* cairo_path_t does not have refcounting. */
  priv->overlay_path = overlay_path;

  meta_shaped_texture_dirty_mask (stex);
}

/**
 * meta_shaped_texture_set_clip_region:
 * @stex: a #MetaShapedTexture
 * @clip_region: (transfer full): the region of the texture that
 *   is visible and should be painted.
 *
 * Provides a hint to the texture about what areas of the texture
 * are not completely obscured and thus need to be painted. This
 * is an optimization and is not supposed to have any effect on
 * the output.
 *
 * Typically a parent container will set the clip region before
 * painting its children, and then unset it afterwards.
 */
void
meta_shaped_texture_set_clip_region (MetaShapedTexture *stex,
				     cairo_region_t    *clip_region)
{
  MetaShapedTexturePrivate *priv;

  g_return_if_fail (META_IS_SHAPED_TEXTURE (stex));

  priv = stex->priv;

  if (priv->clip_region)
    {
      cairo_region_destroy (priv->clip_region);
      priv->clip_region = NULL;
    }

  if (clip_region)
    priv->clip_region = cairo_region_copy (clip_region);
  else
    priv->clip_region = NULL;
}

/**
 * meta_shaped_texture_get_image:
 * @stex: A #MetaShapedTexture
 * @clip: A clipping rectangle, to help prevent extra processing.
 * In the case that the clipping rectangle is partially or fully
 * outside the bounds of the texture, the rectangle will be clipped.
 *
 * Flattens the two layers of the shaped texture into one ARGB32
 * image by alpha blending the two images, and returns the flattened
 * image.
 *
 * Returns: (transfer full): a new cairo surface to be freed with
 * cairo_surface_destroy().
 */
cairo_surface_t *
meta_shaped_texture_get_image (MetaShapedTexture     *stex,
                               cairo_rectangle_int_t *clip)
{
  CoglTexture *texture, *mask_texture;
  cairo_rectangle_int_t texture_rect = { 0, 0, 0, 0 };
  cairo_surface_t *surface;

  g_return_val_if_fail (META_IS_SHAPED_TEXTURE (stex), NULL);

  texture = stex->priv->texture;

  if (texture == NULL)
    return NULL;

  texture_rect.width = cogl_texture_get_width (texture);
  texture_rect.height = cogl_texture_get_height (texture);

  if (clip != NULL)
    {
      /* GdkRectangle is just a typedef of cairo_rectangle_int_t,
       * so we can use the gdk_rectangle_* APIs on these. */
      if (!gdk_rectangle_intersect (&texture_rect, clip, clip))
        return NULL;
    }

  if (clip != NULL)
    texture = cogl_texture_new_from_sub_texture (texture,
                                                 clip->x,
                                                 clip->y,
                                                 clip->width,
                                                 clip->height);

  surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32,
                                        cogl_texture_get_width (texture),
                                        cogl_texture_get_height (texture));

  cogl_texture_get_data (texture, CLUTTER_CAIRO_FORMAT_ARGB32,
                         cairo_image_surface_get_stride (surface),
                         cairo_image_surface_get_data (surface));

  cairo_surface_mark_dirty (surface);

  if (clip != NULL)
    cogl_object_unref (texture);

  mask_texture = stex->priv->mask_texture;
  if (mask_texture != NULL)
    {
      cairo_t *cr;
      cairo_surface_t *mask_surface;

      if (clip != NULL)
        mask_texture = cogl_texture_new_from_sub_texture (mask_texture,
                                                          clip->x,
                                                          clip->y,
                                                          clip->width,
                                                          clip->height);

      mask_surface = cairo_image_surface_create (CAIRO_FORMAT_A8,
                                                 cogl_texture_get_width (mask_texture),
                                                 cogl_texture_get_height (mask_texture));

      cogl_texture_get_data (mask_texture, COGL_PIXEL_FORMAT_A_8,
                             cairo_image_surface_get_stride (mask_surface),
                             cairo_image_surface_get_data (mask_surface));

      cairo_surface_mark_dirty (mask_surface);

      cr = cairo_create (surface);
      cairo_set_source_surface (cr, mask_surface, 0, 0);
      cairo_set_operator (cr, CAIRO_OPERATOR_DEST_IN);
      cairo_paint (cr);
      cairo_destroy (cr);

      cairo_surface_destroy (mask_surface);

      if (clip != NULL)
        cogl_object_unref (mask_texture);
    }

  return surface;
}
