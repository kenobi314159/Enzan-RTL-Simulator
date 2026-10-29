/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What NVC's analysis needs from outside the files Enzan builds of it (docs/nvc_integration.md).
 *
 * Two things. NVC's driver, nvc.c, defines the version string its report of an internal error
 * prints, and Enzan builds no driver. And NVC's simplifier folds locally static expressions through
 * its constant evaluator, eval.c, which lowers them and runs them on NVC's JIT interpreter; that
 * path is most of NVC and is the one Enzan does not use. These two functions are the whole of what
 * the simplifier calls of it, so replacing them is the seam: nothing is folded during analysis, the
 * tree keeps those expressions, and the VHDL adapter folds them as it translates.
 */

#include "util.h"
#include "eval.h"
#include "tree.h"

const char version_string[] = PACKAGE_STRING;

bool eval_possible(tree_t t, unit_registry_t *ur, mir_context_t *mc)
{
   (void)t;
   (void)ur;
   (void)mc;
   return false;
}

tree_t eval_try_fold(jit_t *jit, tree_t expr, unit_registry_t *registry, lower_unit_t *parent,
                     void *context)
{
   (void)jit;
   (void)registry;
   (void)parent;
   (void)context;
   return expr;
}
