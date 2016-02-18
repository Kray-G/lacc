#ifndef HASH_H
#define HASH_H

#include "string.h"

struct hash_table {
    /* Number of slots in the top level array. */
    unsigned capacity;

    /* Retrieve string representing the key we are hashing on. Keys are
     * unique identifiers of the elements, meaning they can be compared
     * for equality. */
    struct string (*key)(void *);

    /* Element initializer, called when data is added to table. New data
     * is only inserted on hash_insert if it does not exist already,
     * where equality is determined by comparing keys. */
    void *(*add)(void *);

    /* Element finalizer, called when data is removed, or table is
     * destroyed. */
    void (*del)(void *);

    /* First level array of entries, of length capacity. Resolve
     * collisions by chaining elements.
     *
     * [A] -> [B]
     * [ ]
     * [C]
     * [ ]
     *
     */
    struct hash_entry *table;
};

/* Initialize hash structure. Must be freed by hash_destroy.
 */
struct hash_table *hash_init(
    struct hash_table *tab,
    unsigned cap,
    struct string (*key)(void *),
    void *(*add)(void *),
    void (*del)(void *));

/* Free resources owned by table.
 */
void hash_destroy(struct hash_table *tab);

/* Insert element, or return existing with the same key.
 */
void *hash_insert(struct hash_table *tab, void *val);

/* Retrieve element matching key, or NULL if not found.
 */
void *hash_lookup(struct hash_table *tab, struct string key);

/* Remove element matching key.
 */
void hash_remove(struct hash_table *tab, struct string key);

#endif
