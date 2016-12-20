/*
 * empathy-rounded-effect.c - Source for EmpathyRoundedEffect
 * Copyright (C) 2011 Collabora Ltd.
 * @author Emilio Pozuelo Monfort <emilio.pozuelo@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "config.h"
#include "empathy-rounded-effect.h"

G_DEFINE_TYPE (EmpathyRoundedEffect,
    empathy_rounded_effect,
    CLUTTER_TYPE_EFFECT)

static void
empathy_rounded_effect_paint (ClutterEffect *effect,
    ClutterEffectPaintFlags flags)
{
  EmpathyRoundedEffect *self = EMPATHY_ROUNDED_EFFECT (effect);
  ClutterActor *actor;
  ClutterActorBox allocation = { 0, };
  gfloat width, height;

  actor = clutter_actor_meta_get_actor (CLUTTER_ACTOR_META (self));
  clutter_actor_get_allocation_box (actor, &allocation);
  clutter_actor_box_get_size (&allocation, &width, &height);

  cogl_path_new ();

  /* Create and store a path describing a rounded rectangle. The small
   * size of the preview window makes the radius of the rounded corners
   * very small too, so we can safely use a very coarse angle step
   * without loosing rendering accuracy. It also significantly reduces
   * the time spent in the underlying internal cogl path functions */
  cogl_path_round_rectangle (0, 0, width, height, height / 16., 15);

  cogl_clip_push_from_path ();

  /* Flip */
  cogl_push_matrix ();
  cogl_translate (width, 0, 0);
  cogl_scale (-1, 1, 1);

  clutter_actor_continue_paint (actor);

  cogl_pop_matrix ();
  cogl_clip_pop ();
}

static void
empathy_rounded_effect_init (EmpathyRoundedEffect *self)
{
}

static void
empathy_rounded_effect_class_init (EmpathyRoundedEffectClass *klass)
{
  ClutterEffectClass *effect_class = CLUTTER_EFFECT_CLASS (klass);

  effect_class->paint = empathy_rounded_effect_paint;
}

ClutterEffect *
empathy_rounded_effect_new (void)
{
  return CLUTTER_EFFECT (
    g_object_new (EMPATHY_TYPE_ROUNDED_EFFECT, NULL));
}
