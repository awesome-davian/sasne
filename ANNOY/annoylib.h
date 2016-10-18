// Copyright (c) 2013 Spotify AB
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy of
// the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#ifndef ANNOYLIB_H
#define ANNOYLIB_H

#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>

//#include "mman.h"
//#include <Windows.h>
#include <sys/mman.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <algorithm>
#include <queue>
#include <limits>

// This allows others to supply their own logger / error printer without
// requiring Annoy to import their headers. See RcppAnnoy for a use case.
#ifndef __ERROR_PRINTER_OVERRIDE__
  #define showUpdate(...) { fprintf(stderr, __VA_ARGS__ ); }
#else
  #define showUpdate(...) { __ERROR_PRINTER_OVERRIDE__( __VA_ARGS__ ); }
#endif

#ifndef ANNOY_NODE_ATTRIBUTE
  #define ANNOY_NODE_ATTRIBUTE __attribute__((__packed__))
  // TODO: this is turned on by default, but may not work for all architectures! Need to investigate.
#endif


using std::vector;
using std::string;
using std::pair;
using std::numeric_limits;
using std::make_pair;

struct RandRandom {
  // Default implementation of annoy-specific random number generator that uses rand() from standard library.
  // Owned by the AnnoyIndex, passed around to the distance metrics
  inline int flip() {
    // Draw random 0 or 1
    return rand() & 1;
  }
  inline size_t index(size_t n) {
    // Draw random integer between 0 and n-1 where n is at most the number of data points you have
    return rand() % n;
  }
};

template<typename T>
inline T get_norm(T* v, int f) {
  T sq_norm = 0;
  for (int z = 0; z < f; z++)
    sq_norm += v[z] * v[z];
  return sqrt(sq_norm);
}

template<typename T>
inline void normalize(T* v, int f) {
  T norm = get_norm(v, f);
  for (int z = 0; z < f; z++)
    v[z] /= norm;
}


template<typename S, typename T, class Random>
struct Angular {
  struct /*ANNOY_NODE_ATTRIBUTE*/ Node {
    /*
     * We store a binary tree where each node has two things
     * - A vector associated with it
     * - Two children
     * All nodes occupy the same amount of memory
     * All nodes with n_descendants == 1 are leaf nodes.
     * A memory optimization is that for nodes with 2 <= n_descendants <= K,
     * we skip the vector. Instead we store a list of all descendants. K is
     * determined by the number of items that fits in the space of the vector.
     * For nodes with n_descendants == 1 the vector is a data point.
     * For nodes with n_descendants > K the vector is the normal of the split plane.
     * Note that we can't really do sizeof(node<T>) because we cheat and allocate
     * more memory to be able to fit the vector outside
     */
    S n_descendants;
    S children[2]; // Will possibly store more than 2
    T v[1]; // We let this one overflow intentionally. Need to allocate at least 1 to make GCC happy
  };
  static inline T distance(const T* x, const T* y, int f) {
    // want to calculate (a/|a| - b/|b|)^2
    // = a^2 / a^2 + b^2 / b^2 - 2ab/|a||b|
    // = 2 - 2cos
    T pp = 0, qq = 0, pq = 0;
    for (int z = 0; z < f; z++, x++, y++) {
      pp += (*x) * (*x);
      qq += (*y) * (*y);
      pq += (*x) * (*y);
    }
    T ppqq = pp * qq;
    if (ppqq > 0) return 2.0 - 2.0 * pq / sqrt(ppqq);
    else return 2.0; // cos is 0
  }
  static inline T margin(const Node* n, const T* y, int f) {
    T dot = 0;
    for (int z = 0; z < f; z++)
      dot += n->v[z] * y[z];
    return dot;
  }
  static inline bool side(const Node* n, const T* y, int f, Random& random) {
    T dot = margin(n, y, f);
    if (dot != 0)
      return (dot > 0);
    else
      return random.flip();
  }
  static inline void create_split(const vector<Node*>& nodes, int f, Random& random, Node* n) {
    // Sample two random points from the set of nodes
    // Calculate the hyperplane equidistant from them
    size_t count = nodes.size();
    size_t i = random.index(count);
    size_t j = random.index(count-1);
    j += (j >= i); // ensure that i != j
    T* iv = nodes[i]->v;
    T* jv = nodes[j]->v;
    T i_norm = get_norm(iv, f);
    T j_norm = get_norm(jv, f);
    for (int z = 0; z < f; z++)
      n->v[z] = iv[z] / i_norm - jv[z] / j_norm;
    normalize(n->v, f);
  }
  static inline T normalized_distance(T distance) {
    // Used when requesting distances from Python layer
    return sqrt(distance);
  }
};

template<typename S, typename T, class Random>
struct Euclidean {
  struct /*ANNOY_NODE_ATTRIBUTE*/ Node {
    S n_descendants;
    T a; // need an extra constant term to determine the offset of the plane
    S children[2];
    T v[1];
  };
  static inline T distance(const T* x, const T* y, int f) {
    T d = 0.0;
    for (int i = 0; i < f; i++, x++, y++)
      d += ((*x) - (*y)) * ((*x) - (*y));
    return d;
  }
  static inline T margin(const Node* n, const T* y, int f) {
    T dot = n->a;
    for (int z = 0; z < f; z++)
      dot += n->v[z] * y[z];
    return dot;
  }
  static inline bool side(const Node* n, const T* y, int f, Random& random) {
    T dot = margin(n, y, f);
    if (dot != 0)
      return (dot > 0);
    else
      return random.flip();
  }
  static inline void create_split(const vector<Node*>& nodes, int f, Random& random, Node* n) {
    // Same as Angular version, but no normalization and has to compute the offset too
    size_t count = nodes.size();
    size_t i = random.index(count);
    size_t j = random.index(count-1);
    j += (j >= i); // ensure that i != j
    T* iv = nodes[i]->v;
    T* jv = nodes[j]->v;
    n->a = 0.0;
    for (int z = 0; z < f; z++) {
      n->v[z] = iv[z] - jv[z];
      n->a += -n->v[z] * (iv[z] + jv[z]) / 2;
    }
  }
  static inline T normalized_distance(T distance) {
    return sqrt(distance);
  }
};

template<typename S, typename T>
class AnnoyIndexInterface {
 public:
  virtual ~AnnoyIndexInterface() {};
  virtual void add_item(S item, const T* w) = 0;
  virtual void build(int q) = 0;
  virtual bool save(const char* filename) = 0;
  virtual void reinitialize() = 0;
  virtual void unload() = 0;
  virtual bool load(const char* filename) = 0;
  virtual T get_distance(S i, S j) = 0;
  virtual void get_nns_by_item(S item, size_t n, size_t search_k, vector<S>* result, vector<T>* distances) = 0;
  virtual void get_nns_by_vector(const T* w, size_t n, size_t search_k, vector<S>* result, vector<T>* distances) = 0;
  virtual S get_n_items() = 0;
  virtual void verbose(bool v) = 0;
  virtual void get_item(S item, vector<T>* v) = 0;
};

template<typename S, typename T, template<typename, typename, typename> class Distance, class Random>
  class AnnoyIndex : public AnnoyIndexInterface<S, T> {
  /*
   * We use random projection to build a forest of binary trees of all items.
   * Basically just split the hyperspace into two sides by a hyperplane,
   * then recursively split each of those subtrees etc.
   * We create a tree like this q times. The default q is determined automatically
   * in such a way that we at most use 2x as much memory as the vectors take.
   */
protected:
  typedef Distance<S, T, Random> D;
  typedef typename D::Node Node;

  int _f;
  size_t _s;
  S _n_items;
  Random _random;
  void* _nodes; // Could either be mmapped, or point to a memory buffer that we reallocate
  S _n_nodes;
  S _nodes_size;
  vector<S> _roots;
  S _K;
  bool _loaded;
  bool _verbose;
public:

  AnnoyIndex(int f) : _random() {
    _f = f;
    _s = offsetof(Node, v) + f * sizeof(T); // Size of each node
    _n_items = 0;
    _n_nodes = 0;
    _nodes_size = 0;
    _nodes = NULL;
    _loaded = false;
    _verbose = false;

    _K = (_s - offsetof(Node, children)) / sizeof(S); // Max number of descendants to fit into node
  }
  ~AnnoyIndex() {
    if (_loaded) {
      unload();
    } else if(_nodes) {
      free(_nodes);
    }
  }

  void add_item(S item, const T* w) {
    _allocate_size(item + 1);
    Node* n = _get(item);

    n->children[0] = 0;
    n->children[1] = 0;
    n->n_descendants = 1;

    for (int z = 0; z < _f; z++)
      n->v[z] = w[z];

    if (item >= _n_items)
      _n_items = item + 1;
  }

  void build(int q) {
    _n_nodes = _n_items;
    while (1) {
      if (q == -1 && _n_nodes >= _n_items * 2)
        break;
      if (q != -1 && _roots.size() >= (size_t)q)
        break;
      if (_verbose) showUpdate("pass %zd...\n", _roots.size());

      vector<S> indices;
      for (S i = 0; i < _n_items; i++)
        indices.push_back(i);

      _roots.push_back(_make_tree(indices));
    }
    // Also, copy the roots into the last segment of the array
    // This way we can load them faster without reading the whole file
    _allocate_size(_n_nodes + (S)_roots.size());
    for (size_t i = 0; i < _roots.size(); i++)
      memcpy(_get(_n_nodes + (S)i), _get(_roots[i]), _s);
    _n_nodes += _roots.size();

    if (_verbose) showUpdate("has %d nodes\n", _n_nodes);
  }

  bool save(const char* filename) {
    FILE *f = fopen(filename, "wb");
    if (f == NULL)
      return false;

    //fwrite(_nodes, _s, _n_nodes, f);
	for (long long i = 0; i < _n_nodes; ++i)
		fwrite((void*)((char*)_nodes + i * _s), _s, 1, f);

    fclose(f);

    free(_nodes);
    _n_items = 0;
    _n_nodes = 0;
    _nodes_size = 0;
    _nodes = NULL;
    _roots.clear();
    return load(filename);
  }

  void reinitialize() {
    _nodes = NULL;
    _loaded = false;
    _n_items = 0;
    _n_nodes = 0;
    _nodes_size = 0;
    _roots.clear();
  }

  void unload() {
    off_t size = _n_nodes * _s;
    munmap(_nodes, size);
    reinitialize();
    if (_verbose) showUpdate("unloaded\n");
  }

  bool load(const char* filename) {
    int fd = open(filename, O_RDONLY, (int)0400);
    if (fd == -1)
      return false;
    long long size = lseek64(fd, 0, SEEK_END);
//	printf("File size %lld\n", size);
#ifdef MAP_POPULATE
    _nodes = (Node*)mmap(
        0, size, PROT_READ, MAP_SHARED | MAP_POPULATE, fd, 0);
#else
    _nodes = (Node*)mmap(
        0, size, PROT_READ, MAP_SHARED, fd, 0);
#endif

    _n_nodes = (S)(size / _s);

    // Find the roots by scanning the end of the file and taking the nodes with most descendants
    S m = -1;
    for (S i = _n_nodes - 1; i >= 0; i--) {
      S k = _get(i)->n_descendants;
      if (m == -1 || k == m) {
        _roots.push_back(i);
        m = k;
      } else {
        break;
      }
    }
    _loaded = true;
    _n_items = m;
    if (_verbose) showUpdate("found %lu roots with degree %d\n", _roots.size(), m);
    return true;
  }

  T get_distance(S i, S j) {
    const T* x = _get(i)->v;
    const T* y = _get(j)->v;
    return D::distance(x, y, _f);
  }

  void get_nns_by_item(S item, size_t n, size_t search_k, vector<S>* result, vector<T>* distances) {
    const Node* m = _get(item);
    _get_all_nns(m->v, n, search_k, result, distances);
  }

  void get_nns_by_vector(const T* w, size_t n, size_t search_k, vector<S>* result, vector<T>* distances) {
    _get_all_nns(w, n, search_k, result, distances);
  }
  S get_n_items() {
    return _n_items;
  }
  void verbose(bool v) {
    _verbose = v;
  }

  void get_item(S item, vector<T>* v) {
    Node* m = _get(item);
    for (int z = 0; z < _f; z++)
      v->push_back(m->v[z]);
  }

protected:
  void _allocate_size(S n) {
    if (n > _nodes_size) {
      S new_nodes_size = (_nodes_size + 1) * 2;
      if (n > new_nodes_size)
        new_nodes_size = n;
      _nodes = realloc(_nodes, _s * new_nodes_size);
      memset((char *)_nodes + (_nodes_size * _s)/sizeof(char), 0, (new_nodes_size - _nodes_size) * _s);
      _nodes_size = new_nodes_size;
    }
  }

  inline Node* _get(S i) {
    return (Node*)((uint8_t *)_nodes + (_s * i));
  }

  S _make_tree(const vector<S >& indices) {
    if (indices.size() == 1)
      return indices[0];

    if (indices.size() <= (size_t)_K) {
      _allocate_size(_n_nodes + 1);
      S item = _n_nodes++;
      Node* m = _get(item);
      m->n_descendants = (S)indices.size();

      // Using std::copy instead of a loop seems to resolve issues #3 and #13,
      // probably because gcc 4.8 goes overboard with optimizations.
      copy(indices.begin(), indices.end(), m->children);
      return item;
    }

    Node* m = (Node*)malloc(_s); // TODO: avoid

    vector<S> children_indices[2];
    for (int attempt = 0; attempt < 20; attempt ++) {
      /*
       * Create a random hyperplane.
       * If all points end up on the same time, we try again.
       * We could in principle *construct* a plane so that we split
       * all items evenly, but I think that could violate the guarantees
       * given by just picking a hyperplane at random
       */
      vector<Node*> children;

      for (size_t i = 0; i < indices.size(); i++) {
        // TODO: this loop isn't needed for the angular distance, because
        // we can just split by a random vector and it's fine. For Euclidean
        // distance we need it to calculate the offset
        S j = indices[i];
        Node* n = _get(j);
        if (n)
          children.push_back(n);
      }

      D::create_split(children, _f, _random, m);

      children_indices[0].clear();
      children_indices[1].clear();

      for (size_t i = 0; i < indices.size(); i++) {
        S j = indices[i];
        Node* n = _get(j);
        if (n) {
          bool side = D::side(m, n->v, _f, _random);
          children_indices[side].push_back(j);
        }
      }

      if (children_indices[0].size() > 0 && children_indices[1].size() > 0) {
        break;
      }
    }

    while (children_indices[0].size() == 0 || children_indices[1].size() == 0) {
      // If we didn't find a hyperplane, just randomize sides as a last option
      if (_verbose && indices.size() > 100000)
        showUpdate("Failed splitting %lu items\n", indices.size());

      children_indices[0].clear();
      children_indices[1].clear();

      // Set the vector to 0.0
      for (int z = 0; z < _f; z++)
        m->v[z] = 0.0;

      for (size_t i = 0; i < indices.size(); i++) {
        S j = indices[i];
        // Just randomize...
        children_indices[_random.flip()].push_back(j);
      }
    }

    int flip = (children_indices[0].size() > children_indices[1].size());

    m->n_descendants = (S)indices.size();
    for (int side = 0; side < 2; side++)
      // run _make_tree for the smallest child first (for cache locality)
      m->children[side^flip] = _make_tree(children_indices[side^flip]);

    _allocate_size(_n_nodes + 1);
    S item = _n_nodes++;
    memcpy(_get(item), m, _s);
    free(m);

    return item;
  }

  void _get_all_nns(const T* v, size_t n, size_t search_k, vector<S>* result, vector<T>* distances) {
    std::priority_queue<pair<T, S> > q;

    if (search_k == (size_t)-1)
      search_k = n * _roots.size(); // slightly arbitrary default value

    for (size_t i = 0; i < _roots.size(); i++) {
      q.push(make_pair(numeric_limits<T>::infinity(), _roots[i]));
    }

    vector<S> nns;
    while (nns.size() < search_k && !q.empty()) {
      const pair<T, S>& top = q.top();
      T d = top.first;
      S i = top.second;
      Node* nd = _get(i);
      q.pop();
      if (nd->n_descendants == 1) {
        nns.push_back(i);
      } else if (nd->n_descendants <= _K) {
        const S* dst = nd->children;
        nns.insert(nns.end(), dst, &dst[nd->n_descendants]);
      } else {
        T margin = D::margin(nd, v, _f);
        q.push(make_pair((std::min)(d, +margin), nd->children[1]));
		q.push(make_pair((std::min)(d, -margin), nd->children[0]));
      }
    }

    // Get distances for all items
    // To avoid calculating distance multiple times for any items, sort by id
    sort(nns.begin(), nns.end());
    vector<pair<T, S> > nns_dist;
    S last = -1;
    for (size_t i = 0; i < nns.size(); i++) {
      S j = nns[i];
      if (j == last)
        continue;
      last = j;
      nns_dist.push_back(make_pair(D::distance(v, _get(j)->v, _f), j));
    }

    size_t m = nns_dist.size();
    size_t p = n < m ? n : m; // Return this many items
    std::partial_sort(&nns_dist[0], &nns_dist[p], &nns_dist[m]);
    for (size_t i = 0; i < p; i++) {
      if (distances)
	distances->push_back(D::normalized_distance(nns_dist[i].first));
      result->push_back(nns_dist[i].second);
    }
  }
};

#endif
// vim: tabstop=2 shiftwidth=2
