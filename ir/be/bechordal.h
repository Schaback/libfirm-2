
/**
 * Chordal register allocation.
 * @author Sebastian Hack
 * @date 14.12.2004
 */

#ifndef __BECHORDAL_H
#define __BECHORDAL_H

#include "irgraph.h"
#include "irnode.h"

/**
 * Allocate registers for an ir graph.
 * @param irg The graph.
 * @return Some internal data to be freed with be_ra_chordal_free().
 */
void be_ra_chordal(ir_graph *irg);

void be_ra_chordal_done(ir_graph *irg);

int phi_ops_interfere(const ir_node *a, const ir_node *b);

#endif
