/*
 *
  ***** BEGIN LICENSE BLOCK *****
 
  Copyright (C) 2009-2017 Olof Hagsand and Benny Holmgren

  This file is part of CLIXON.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  Alternatively, the contents of this file may be used under the terms of
  the GNU General Public License Version 3 or later (the "GPL"),
  in which case the provisions of the GPL are applicable instead
  of those above. If you wish to allow use of your version of this file only
  under the terms of the GPL, and not to allow others to
  use your version of this file under the terms of Apache License version 2, 
  indicate your decision by deleting the provisions above and replace them with
  the  notice and other provisions required by the GPL. If you do not delete
  the provisions above, a recipient may use your version of this file under
  the terms of any one of the Apache License version 2 or the GPL.

  ***** END LICENSE BLOCK *****

 */


/*
 * A simple implementation of a associative array style data store. Keys
 * are always strings while values can be some arbitrary data referenced
 * by void*.
 *
 * XXX: functions such as hash_keys(), hash_value() etc are currently returning
 * pointers to the actual data storage. Should probably make copies.
 *
 * Example usage:
 *
 *  int main()
 *  {
 *    char **s;
 *    int n;
 *    size_t slen;
 *    clicon_hash_t *hash = hash_init();
 *
 *    n = 234;
 *    hash_add (hash, "APA", &n, sizeof(n));
 *    hash_dump(hash, stdout);
 *
 *    puts("----");
 *
 *    hash_add (hash, "BEPA", "hoppla Polle!", strlen("hoppla Polle!")+1);
 *    puts((char *)hash_value(hash, "BEPA", NULL));
 *    hash_dump(hash, stdout);
 *    
 *    puts("----");
 *
 *    n = 33;
 *    hash_add (hash, "CEPA", &n, sizeof(n));
 *    hash_dump(hash, stdout);
 *
 *    puts("----");
 *    
 *    hash_del (hash, "APA");
 *    hash_dump(hash, stdout);
 *
 *    hash_free(hash);
 *
 *    return 0;
 * }
 */

#ifdef HAVE_CONFIG_H
#include "clixon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

/* clicon */
#include "clixon_queue.h"
#include "clixon_err.h"
#include "clixon_hash.h"

#define HASH_SIZE	1031	/* Number of hash buckets. Should be a prime */ 

/*! A very simplistic algorithm to calculate a hash bucket index
 */
static uint32_t
hash_bucket(const char *str)
{
    uint32_t n = 0;

    while(*str)
	n += (uint32_t)*str++;
    
    return n % HASH_SIZE;
}

/*! Initialize hash table.
 *
 * @retval Pointer to new hash table.
 */
clicon_hash_t *
hash_init (void)
{
  clicon_hash_t *hash;

  if ((hash = (clicon_hash_t *)malloc (sizeof (clicon_hash_t) * HASH_SIZE)) == NULL){
      clicon_err(OE_UNIX, errno, "malloc: %s", strerror(errno));
      return NULL;
  }
  memset (hash, 0, sizeof(clicon_hash_t)*HASH_SIZE);
  return hash;
}

/*! Free hash table.
 *
 * @param[in] hash	  Hash table
 * @retval    void
 */
void
hash_free(clicon_hash_t *hash)
{
    int i;
    clicon_hash_t tmp;
    for (i = 0; i < HASH_SIZE; i++) {
	while (hash[i]) {
	    tmp = hash[i];
	    DELQ(tmp, hash[i], clicon_hash_t);
	    free(tmp->h_key);
	    free(tmp->h_val);
	    free(tmp);
	}
    }
    free(hash);
}


/*! Find hash key.
 *
 * @param[in] hash     Hash table
 * @param[in] key      Variable name
 * @retval    variable Hash variable structure on success
 * @retval    NULL     Error
 */
clicon_hash_t
hash_lookup(clicon_hash_t *hash, 
	    const char    *key)
{
    uint32_t bkt;
    clicon_hash_t h;

    bkt = hash_bucket(key);
    h = hash[bkt];
    if (h) {
	do {
	    if (!strcmp (h->h_key, key))
		return h;
	    h = NEXTQ(clicon_hash_t, h);
	} while (h != hash[bkt]);
    }
    return NULL;
}

/*! Get value of hash
 * @param[in]  hash Hash table
 * @param[in]  key  Variable name
 * @param[out] vlen Length of value (as returned by function)
 * @retval value    Hash value, length given in vlen
 * @retval NULL     Error
 */
void *
hash_value(clicon_hash_t *hash, 
	   const char    *key, 
	   size_t        *vlen)
{
    clicon_hash_t h;

    h = hash_lookup(hash, key);
    if (h == NULL)
	return NULL;

    if (vlen)
	*vlen = h->h_vlen;
    return h->h_val;
}

/*! Copy value and add hash entry.
 *
 * @param[in] hash   Hash table
 * @param[in] key    New variable name
 * @param[in] val    New variable value
 * @param[in] vlen   Length of variable value
 * @retval variable  New hash structure on success
 * @retval NULL      Failure
 */
clicon_hash_t
hash_add(clicon_hash_t *hash, 
	 const char    *key, 
	 void          *val, 
	 size_t         vlen)
{
    void *newval;
    clicon_hash_t h, new = NULL;
    
    /* If variable exist, don't allocate a new. just replace value */
    h = hash_lookup (hash, key);
    if (h == NULL) {
	if ((new = (clicon_hash_t)malloc (sizeof (*new))) == NULL){
	    clicon_err(OE_UNIX, errno, "malloc: %s", strerror(errno));
	    goto catch;
	}
	memset (new, 0, sizeof (*new));
	
	new->h_key = strdup (key);
	if (new->h_key == NULL){
	    clicon_err(OE_UNIX, errno, "strdup: %s", strerror(errno));
	    goto catch;
	}
	
	h = new;
    }
    
    /* Make copy of lvalue */
    newval = malloc (vlen+3); /* XXX: qdbm needs aligned mallocs? */
    if (newval == NULL){
	clicon_err(OE_UNIX, errno, "malloc: %s", strerror(errno));
	goto catch;
    }
    memcpy (newval, val, vlen);
    
    /* Free old value if existing variable */
    if (h->h_val)
	free (h->h_val);
    h->h_val = newval;
    h->h_vlen =  vlen;

    /* Add to list only if new variable */
    if (new)
	INSQ(h, hash[hash_bucket(key)]);

    return h;

catch:
    if (new) {
	if (new->h_key)
	    free (new->h_key);
	free (new);
    }

    return NULL;
}

/*! Delete hash entry.
 *
 * @param[in] hash    Hash table
 * @param[in] key     Variable name
 *
 * @retval   0  success
 * @retval  -1  failure
 */
int
hash_del(clicon_hash_t *hash, 
	 const char    *key)
{
    clicon_hash_t h;

    h = hash_lookup (hash, key);
    if (h == NULL)
	return -1;
    
    DELQ(h, hash[hash_bucket(key)], clicon_hash_t);
  
    free (h->h_key);
    free (h->h_val);
    free (h);

    return 0;
}

/*! Return vector of keys in has table
 *
 * @param[in]   hash  	Hash table
 * @param[out]	nkeys   Size of key vector
 * @retval      vector  Vector of keys
 * @retval      NULL    Error
 */
char **
hash_keys(clicon_hash_t *hash, 
	  size_t        *nkeys)
{
    int bkt;
    clicon_hash_t h;
    char **tmp;
    char **keys = NULL;

    *nkeys = 0;
    for (bkt = 0; bkt < HASH_SIZE; bkt++) {
	h = hash[bkt];
	do {
	    if (h == NULL)
		break;
	    tmp = realloc(keys, ((*nkeys)+1) * sizeof(char *));
	    if (tmp == NULL){
		clicon_err(OE_UNIX, errno, "realloc: %s", strerror(errno));
		goto catch;
	    }
	    keys = tmp;
	    keys[*nkeys] = h->h_key;
	    (*nkeys)++;
	    h = NEXTQ(clicon_hash_t, h);
	} while (h != hash[bkt]);
    }

    return keys;

catch:
    if (keys)
	free(keys);
    return NULL;
}

/*! Dump contents of hash to FILE pointer.
 *
 * @param[in]   hash  	Hash structure
 * @param[in]	f	FILE pointer for print output
 * @retval      void
 */
void
hash_dump(clicon_hash_t *hash, 
	  FILE          *f)
{
    int i;
    char **keys;
    void *val;
    size_t klen;
    size_t vlen;
    
    if (hash == NULL)
	return;
    keys = hash_keys(hash, &klen);
    if (keys == NULL)
	return;
	
    for(i = 0; i < klen; i++) {
	val = hash_value(hash, keys[i], &vlen);
	printf("%s =\t 0x%p , length %zu\n", keys[i], val, vlen);
    }
    free(keys);
}
