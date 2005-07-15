/**
 * Register allocation functions.
 * @author Sebastian Hack
 * @date 13.1.2005
 */

#ifndef _BERA_H
#define _BERA_H

#include "irnode.h"

/**
 * Check, if two values interfere.
 * @param a The first value.
 * @param b The second value.
 * @return 1, if @p a and @p b interfere, 0 if not.
 */
int values_interfere(const ir_node *a, const ir_node *b);

/**
 * Check, if a value dominates the other one.
 * Note, that this function also consideres the schedule and does thus
 * more than block_dominates().
 *
 * @param a The first.
 * @param b The second value.
 * @return 1 if a dominates b, 0 else.
 */
int value_dominates(const ir_node *a, const ir_node *b);


#endif /* _BERA_H */
