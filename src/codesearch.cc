/********************************************************************
 * livegrep -- codesearch.cc
 * Copyright (c) 2011-2013 Nelson Elhage
 *
 * This program is free software. You may use, redistribute, and/or
 * modify it under the terms listed in the COPYING file.
 ********************************************************************/
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include <locale>
#include <list>
#include <iostream>
#include <string>
#include <fstream>
#include <limits>
#include <atomic>

#include <re2/re2.h>
#include <gflags/gflags.h>
#include <openssl/sha.h>

#include "timer.h"
#include "metrics.h"
#include "thread_queue.h"
#include "codesearch.h"
#include "chunk.h"
#include "chunk_allocator.h"
#include "radix_sort.h"
#include "indexer.h"
#include "per_thread.h"
#include "debug.h"
#include "content.h"

#include "utf8.h"

using re2::RE2;
using re2::StringPiece;
using namespace std;

const int    kContextLines = 3;

const size_t kMinSkip = 250;
const int kMinFilterRatio = 50;
const int kMaxScan        = (1 << 20);

DEFINE_bool(index, true, "Create a suffix-array index to speed searches.");
DEFINE_bool(drop_cache, false, "Drop caches before each search");
DEFINE_bool(search, true, "Actually do the search.");
DEFINE_int32(max_matches, 50, "The maximum number of results to return for a single query.");
DEFINE_int32(timeout, 1000, "The number of milliseconds a single search may run for.");
DEFINE_int32(threads, 4, "Number of threads to use.");
DEFINE_int32(line_limit, 1024, "Maximum line length to index.");

namespace {
    metric idx_bytes("index.bytes");
    metric idx_bytes_dedup("index.bytes.dedup");
    metric idx_files("index.files");
    metric idx_lines("index.lines");
    metric idx_lines_dedup("index.lines.dedup");
    metric idx_data_chunks("index.data.chunks");
    metric idx_content_chunks("index.content.chunks");
    metric idx_content_ranges("index.content.ranges");
    metric idx_hash_time("timer.index.dedup.hash");
};

bool eqstr::operator()(const StringPiece& lhs, const StringPiece& rhs) const {
    if (lhs.data() == NULL && rhs.data() == NULL)
        return true;
    if (lhs.data() == NULL || rhs.data() == NULL)
        return false;
    return lhs == rhs;
}

size_t hashstr::operator()(const StringPiece& str) const {
    const std::collate<char>& coll = std::use_facet<std::collate<char> >(loc);
    return coll.hash(str.data(), str.data() + str.size());
}

bool operator==(const sha1_buf &lhs, const sha1_buf &rhs) {
    return memcmp(lhs.hash, rhs.hash, sizeof(lhs.hash)) == 0;
}

void sha1_string(sha1_buf *out, StringPiece string) {
    SHA_CTX ctx;
    SHA1_Init(&ctx);
    SHA1_Update(&ctx, string.data(), string.size());
    SHA1_Final(out->hash, &ctx);
}

size_t hash_sha1::operator()(const sha1_buf& hash) const {
    /*
     * We could hash the entire oid together, but since the oid is the
     * output of a cryptographic hash anyways, just taking the first N
     * bytes should work just well.
     */
    union {
        sha1_buf sha1;
        size_t trunc;
    } u = {hash};
    return u.trunc;
}

const StringPiece empty_string(NULL, 0);

class code_searcher;
struct match_finger;

class searcher {
public:
    searcher(const code_searcher *cc, const query &q) :
        cc_(cc), query_(&q), queue_(),
        matches_(0), re2_time_(false), git_time_(false),
        index_time_(false), sort_time_(false), analyze_time_(false),
        exit_reason_(kExitNone), files_(new uint8_t[cc->files_.size()]),
        files_density_(-1)
    {
        memset(files_, 0xff, cc->files_.size());
        {
            run_timer run(analyze_time_);
            index_ = indexRE(*query_->line_pat);
        }

        if (FLAGS_timeout <= 0) {
            limit_.tv_sec = numeric_limits<time_t>::max();
        } else {
            timeval timeout = {
                0, FLAGS_timeout * 1000
            }, now;
            gettimeofday(&now, NULL);
            timeval_add(&limit_, &now, &timeout);
        }
    }

    ~searcher() {
        delete[] files_;

        debug(kDebugProfile, "re2 time: %d.%06ds",
              int(re2_time_.elapsed().tv_sec),
              int(re2_time_.elapsed().tv_usec));
        debug(kDebugProfile, "git time: %d.%06ds",
              int(git_time_.elapsed().tv_sec),
              int(git_time_.elapsed().tv_usec));
        debug(kDebugProfile, "index time: %d.%06ds",
              int(index_time_.elapsed().tv_sec),
              int(index_time_.elapsed().tv_usec));
        debug(kDebugProfile, "sort time: %d.%06ds",
              int(sort_time_.elapsed().tv_sec),
              int(sort_time_.elapsed().tv_usec));
        debug(kDebugProfile, "analyze time: %d.%06ds",
              int(analyze_time_.elapsed().tv_sec),
              int(analyze_time_.elapsed().tv_usec));
    }

    void operator()(const chunk *chunk);

    void get_stats(match_stats *stats) {
        stats->re2_time = re2_time_.elapsed();
        stats->git_time = git_time_.elapsed();
        stats->index_time = index_time_.elapsed();
        stats->sort_time  = sort_time_.elapsed();
        stats->analyze_time  = analyze_time_.elapsed();
    }

    exit_reason why() {
        return exit_reason_;
    }

protected:
    void next_range(match_finger *finger, int& minpos, int& maxpos, int end);
    void full_search(const chunk *chunk);
    void full_search(match_finger *finger, const chunk *chunk,
                     size_t minpos, size_t maxpos);

    void filtered_search(const chunk *chunk);
    void search_lines(uint32_t *left, int count, const chunk *chunk);

    bool accept(const indexed_file *file) {
        if (!query_->file_pat && !query_->tree_pat)
            return true;

        if (query_->file_pat &&
            !query_->file_pat->Match(file->path, 0, file->path.size(),
                                     RE2::UNANCHORED, 0, 0))
            return false;

        if (query_->tree_pat &&
            !query_->tree_pat->Match(file->tree->name, 0,
                                     file->tree->name.size(),
                                     RE2::UNANCHORED, 0, 0))
            return false;

        return true;
    }

    bool accept(const list<indexed_file *> &sfs) {
        for (list<indexed_file *>::const_iterator it = sfs.begin();
             it != sfs.end(); ++it) {
            if (accept(*it))
                return true;
        }
        return false;
    }

    double files_density(void) {
        std::unique_lock<std::mutex> locked(mtx_);
        if (files_density_ >= 0)
            return files_density_;

        int hits = 0;
        int sample = min(1000, int(cc_->files_.size()));
        for (int i = 0; i < sample; i++) {
            if (accept(cc_->files_[rand() % cc_->files_.size()]))
                hits++;
        }
        return (files_density_ = double(hits) / sample);
    }

    /*
     * Do a linear walk over chunk->files, searching for all files
     * which contain `match', which is contained within `line'.
     */
    void find_match_brute(const chunk *chunk,
                          const StringPiece& match,
                          const StringPiece& line);

    /*
     * Given a match `match', contained within `line', find all files
     * that contain that match. If indexing is enabled, do this by
     * walking the chunk_file BST; Otherwise, fall back on a
     * brute-force linear walk.
     */
    void find_match(const chunk *chunk,
                    const StringPiece& match,
                    const StringPiece& line);

    /*
     * Given a matching substring, its containing line, and a search
     * file, determine whether that file actually contains that line,
     * and if so, post results to queue_
     */
    void try_match(const StringPiece&,
                   const StringPiece&,
                   indexed_file *);

    static int line_start(const chunk *chunk, int pos) {
        const unsigned char *start = static_cast<const unsigned char*>
            (memrchr(chunk->data, '\n', pos));
        if (start == NULL)
            return 0;
        return start - chunk->data;
    }

    static int line_end(const chunk *chunk, int pos) {
        const unsigned char *end = static_cast<const unsigned char*>
            (memchr(chunk->data + pos, '\n', chunk->size - pos));
        if (end == NULL)
            return chunk->size;
        return end - chunk->data;
    }

    static StringPiece find_line(const StringPiece& chunk, const StringPiece& match) {
        const char *start, *end;
        assert(match.data() >= chunk.data());
        assert(match.data() <= chunk.data() + chunk.size());
        assert(match.size() <= (chunk.size() - (match.data() - chunk.data())));
        start = static_cast<const char*>
            (memrchr(chunk.data(), '\n', match.data() - chunk.data()));
        if (start == NULL)
            start = chunk.data();
        else
            start++;
        end = static_cast<const char*>
            (memchr(match.data() + match.size(), '\n',
                    chunk.size() - (match.data() - chunk.data()) - match.size()));
        if (end == NULL)
            end = chunk.data() + chunk.size();
        return StringPiece(start, end - start);
    }

    bool exit_early() {
        if (exit_reason_)
            return true;

        if (FLAGS_max_matches && matches_.load() >= FLAGS_max_matches) {
            exit_reason_ = kExitMatchLimit;
            return true;
        }
#ifdef CODESEARCH_SLOWGTOD
        static int counter = 1000;
        if (--counter)
            return false;
        counter = 1000;
#endif
        timeval now;
        gettimeofday(&now, NULL);
        if (now.tv_sec > limit_.tv_sec ||
            (now.tv_sec == limit_.tv_sec && now.tv_usec > limit_.tv_usec)) {
            exit_reason_ = kExitTimeout;
            return true;
        }
        return false;
    }

    const code_searcher *cc_;
    const query *query_;
    thread_queue<match_result*> queue_;
    atomic_int matches_;
    intrusive_ptr<IndexKey> index_;
    timer re2_time_;
    timer git_time_;
    timer index_time_;
    timer sort_time_;
    timer analyze_time_;
    timeval limit_;
    exit_reason exit_reason_;
    uint8_t *files_;

    /*
     * The approximate ratio of how many files match file_pat and
     * tree_pat. Lazily computed -- -1 means it hasn't been computed
     * yet. Protected by mtx_.
     */
    double files_density_;
    std::mutex mtx_;

    friend class code_searcher::search_thread;
};

code_searcher::code_searcher()
    : alloc_(0), finalized_(false)
{
#ifdef USE_DENSE_HASH_SET
    lines_.set_empty_key(empty_string);
#endif
}

void code_searcher::set_alloc(chunk_allocator *alloc) {
    assert(!alloc_);
    alloc_ = alloc;
}

code_searcher::~code_searcher() {
    if (alloc_)
        alloc_->cleanup();
    delete alloc_;
}

void code_searcher::finalize() {
    assert(!finalized_);
    finalized_ = true;
    alloc_->finalize();
    idx_data_chunks.inc(alloc_->end() - alloc_->begin());
    idx_content_chunks.inc(alloc_->end_content() - alloc_->begin_content());
}

vector<indexed_tree> code_searcher::trees() const {
    vector<indexed_tree> out;
    out.reserve(trees_.size());
    for (auto it = trees_.begin(); it != trees_.end(); ++it)
        out.push_back(**it);
    return out;
}

const indexed_tree* code_searcher::open_tree(const string &name,
                                             json_object *metadata,
                                             const string &version) {
    indexed_tree *tree = new indexed_tree;
    tree->name = name;
    tree->version = version;
    if (metadata) {
        tree->metadata = metadata;
    } else {
        tree->metadata = NULL;
    }
    trees_.push_back(tree);
    return tree;
}

void code_searcher::index_file(const indexed_tree *tree,
                               const string& path,
                               StringPiece contents) {
    assert(!finalized_);
    assert(alloc_);
    size_t len = contents.size();
    const char *p = contents.data();
    const char *end = p + len;
    const char *f;
    chunk *c;
    StringPiece line;

    if (memchr(p, 0, len) != NULL)
        return;

    idx_bytes.inc(len);
    idx_files.inc();

    indexed_file *sf = new indexed_file;
    sf->tree = tree;
    sf->path = path;
    sf->no  = files_.size();
    files_.push_back(sf);

    uint32_t lines = count(p, end, '\n');

    // sf->content = new(new uint32_t[3*lines+1]) file_contents(0);
    file_contents_builder content;

    while ((f = static_cast<const char*>(memchr(p, '\n', end - p))) != 0) {
    final:
        idx_lines.inc();
        if (f - p + 1 >= FLAGS_line_limit) {
            // Don't index the long line, but do index an empty
            // line so that line number of future lines are
            // preserved.
            p = f;
        }
        string_hash::iterator it;
        {
            metric::timer tm(idx_hash_time);
            it = lines_.find(StringPiece(p, f - p));
        }
        if (it == lines_.end()) {
            idx_bytes_dedup.inc((f - p) + 1);
            idx_lines_dedup.inc();

            unsigned char *alloc = alloc_->alloc(f - p + 1);
            memcpy(alloc, p, f - p);
            alloc[f - p] = '\n';
            line = StringPiece((char*)alloc, f - p);
            lines_.insert(line);
            c = alloc_->current_chunk();
        } else {
            line = *it;
            c = alloc_->chunk_from_string
                (reinterpret_cast<const unsigned char*>(line.data()));
        }
        c->add_chunk_file(sf, line);
        content.extend(c, line);
        p = min(end, f + 1);
    }
    if (p < end - 1) {
        // Handle files with no trailing newline by jumping back and
        // adding the final line.
        assert(*(end-1) != '\n');
        f = end;
        lines++;
        goto final;
    }

    sf->content = content.build(alloc_);
    if (sf->content == 0) {
        fprintf(stderr, "WARN: %s:%s:%s is too large to be indexed.\n",
                tree->name.c_str(), tree->version.c_str(), path.c_str());
        file_contents_builder dummy;
        sf->content = dummy.build(alloc_);
    }
    idx_content_ranges.inc(sf->content->size());
    assert(sf->content->size() <= 3*lines);

    for (auto it = alloc_->begin();
         it != alloc_->end(); it++)
        (*it)->finish_file();
}

void searcher::operator()(const chunk *chunk)
{
    if (exit_reason_)
        return;

    if (FLAGS_index && index_ && !index_->empty())
        filtered_search(chunk);
    else
        full_search(chunk);
}

struct walk_state {
    uint32_t *left, *right;
    intrusive_ptr<IndexKey> key;
    int depth;
};

struct lt_index {
    const chunk *chunk_;
    int idx_;

    bool operator()(uint32_t lhs, unsigned char rhs) {
        return cmp(lhs, rhs) < 0;
    }

    bool operator()(unsigned char lhs, uint32_t rhs) {
        return cmp(rhs, lhs) > 0;
    }

    int cmp(uint32_t lhs, unsigned char rhs) {
        unsigned char lc = chunk_->data[lhs + idx_];
        if (lc == '\n')
            return -1;
        return (int)lc - (int)rhs;
    }
};


void searcher::filtered_search(const chunk *chunk)
{
    static per_thread<vector<uint32_t> > indexes;
    if (!indexes.get()) {
        indexes.put(new vector<uint32_t>(cc_->alloc_->chunk_size() / kMinFilterRatio));
    }
    int count = 0;
    {
        run_timer run(index_time_);
        vector<walk_state> stack;
        stack.push_back((walk_state){
                chunk->suffixes, chunk->suffixes + chunk->size, index_, 0});

        while (!stack.empty()) {
            walk_state st = stack.back();
            stack.pop_back();
            if (!st.key || st.key->empty() || (st.right - st.left) <= 100) {
                if ((count + st.right - st.left) > indexes->size()) {
                    count = indexes->size() + 1;
                    break;
                }
                memcpy(&(*indexes)[count], st.left,
                       (st.right - st.left) * sizeof(uint32_t));
                count += (st.right - st.left);
                continue;
            }
            lt_index lt = {chunk, st.depth};
            for (IndexKey::iterator it = st.key->begin();
                 it != st.key->end(); ++it) {
                uint32_t *l, *r;
                l = lower_bound(st.left, st.right, it->first.first, lt);
                uint32_t *right = lower_bound(l, st.right,
                                              (unsigned char)(it->first.second + 1),
                                              lt);
                if (l == right)
                    continue;

                if (st.depth)
                    assert(chunk->data[*l + st.depth - 1] ==
                           chunk->data[*(right - 1) + st.depth - 1]);

                assert(l == st.left ||
                       chunk->data[*(l-1) + st.depth] == '\n' ||
                       chunk->data[*(l-1) + st.depth] < it->first.first);
                assert(chunk->data[*l + st.depth] >= it->first.first);
                assert(right == st.right ||
                       chunk->data[*right + st.depth] > it->first.second);

                for (unsigned char ch = it->first.first; ch <= it->first.second;
                     ch++, l = r) {
                    r = lower_bound(l, right, (unsigned char)(ch + 1), lt);

                    if (r != l) {
                        stack.push_back((walk_state){l, r, it->second, st.depth + 1});
                    }
                }
            }
        }
    }

    search_lines(&(*indexes)[0], count, chunk);
}

struct match_finger {
    const chunk *chunk_;
    vector<chunk_file>::const_iterator it_;
    match_finger(const chunk *chunk) :
        chunk_(chunk), it_(chunk->files.begin()) {};
};

void searcher::search_lines(uint32_t *indexes, int count,
                            const chunk *chunk)
{
    debug(kDebugProfile, "search_lines: Searching %d/%d indexes.", count, chunk->size);

    if (count == 0)
        return;

    if (count * kMinFilterRatio > chunk->size) {
        full_search(chunk);
        return;
    }

    if ((query_->file_pat || query_->tree_pat) &&
        double(count * 30) / chunk->size > files_density()) {
        full_search(chunk);
        return;
    }

    {
        run_timer run(sort_time_);
        lsd_radix_sort(indexes, indexes + count);
    }

    match_finger finger(chunk);

    StringPiece search((char*)chunk->data, chunk->size);
    uint32_t max = indexes[0];
    uint32_t min = line_start(chunk, indexes[0]);
    for (int i = 0; i <= count && !exit_early(); i++) {
        if (i != count) {
            if (indexes[i] < max) continue;
            if (indexes[i] < max + kMinSkip) {
                max = indexes[i];
                continue;
            }
        }

        int end = line_end(chunk, max);
        full_search(&finger, chunk, min, end);

        if (i != count) {
            max = indexes[i];
            min = line_start(chunk, max);
        }
    }
}

void searcher::full_search(const chunk *chunk)
{
    match_finger finger(chunk);
    full_search(&finger, chunk, 0, chunk->size - 1);
}

void searcher::next_range(match_finger *finger,
                          int& pos, int& endpos, int maxpos)
{
    if ((!query_->file_pat && !query_->tree_pat) || !FLAGS_index)
        return;

    debug(kDebugSearch, "next_range(%d, %d, %d)", pos, endpos, maxpos);

    vector<chunk_file>::const_iterator& it = finger->it_;
    const vector<chunk_file>::const_iterator& end = finger->chunk_->files.end();

    /* Find the first matching range that intersects [pos, maxpos) */
    while (it != end &&
           (it->right < pos || !accept(it->files)) &&
           it->left < maxpos)
        ++it;

    if (it == end || it->left >= maxpos) {
        pos = endpos = maxpos;
        return;
    }

    pos    = max(pos, it->left);
    endpos = it->right;

    /*
     * Now scan until we either:
     * - prove that [pos, maxpos) is all in range,
     * - find a gap greater than kMinSkip, or
     * - pass maxpos entirely.
     */
    do {
        if (it->left >= endpos + kMinSkip)
            break;
        if (it->right >= endpos && accept(it->files)) {
            endpos = max(endpos, it->right);
            if (endpos >= maxpos)
                /*
                 * We've accepted the entire range. No point in going on.
                 */
                break;
        }
        ++it;
    } while (it != end && it->left < maxpos);

    endpos = min(endpos, maxpos);
}

void searcher::full_search(match_finger *finger,
                           const chunk *chunk, size_t minpos, size_t maxpos)
{
    StringPiece str((char*)chunk->data, chunk->size);
    StringPiece match;
    int pos = minpos, new_pos, end = minpos;
    while (pos < maxpos && !exit_early()) {
        if (pos >= end) {
            end = maxpos;
            next_range(finger, pos, end, maxpos);
            assert(pos <= end);
        }
        if (pos >= maxpos)
            break;

        debug(kDebugSearch, "[%p] range:%d-%d/%d-%d",
              (void*)(chunk), pos, end, int(minpos), int(maxpos));

        {
            int limit = end;
            if (limit - pos > kMaxScan)
                limit = line_end(chunk, pos + kMaxScan);
            run_timer run(re2_time_);
            if (!query_->line_pat->Match(str, pos, limit, RE2::UNANCHORED, &match, 1)) {
                pos = limit + 1;
                continue;
            }
        }
        assert(memchr(match.data(), '\n', match.size()) == NULL);
        StringPiece line = find_line(str, match);
        if (utf8::is_valid(line.data(), line.data() + line.size()))
            find_match(chunk, match, line);
        new_pos = line.size() + line.data() - str.data() + 1;
        assert(new_pos > pos);
        pos = new_pos;
    }
}

void searcher::find_match_brute(const chunk *chunk,
                                const StringPiece& match,
                                const StringPiece& line) {
    run_timer run(git_time_);
    timer tm;
    int off = (unsigned char*)line.data() - chunk->data;
    int searched = 0;

    for(vector<chunk_file>::const_iterator it = chunk->files.begin();
        it != chunk->files.end(); it++) {
        if (off >= it->left && off <= it->right) {
            for (list<indexed_file *>::const_iterator fit = it->files.begin();
                 fit != it->files.end(); ++fit) {
                if (!accept(*fit))
                    continue;
                searched++;
                if (exit_early())
                    break;
                try_match(line, match, *fit);
            }
        }
    }

    tm.pause();
    debug(kDebugProfile, "Searched %d files in %d.%06ds",
          searched,
          int(tm.elapsed().tv_sec),
          int(tm.elapsed().tv_usec));
}

namespace {
    /*
     * We use an explicit stack instead of direct recursion. We
     * want to do an inorder traversal, so that we produce results
     * in ascending order of position in the chunk, so we have two
     * types of frames we can push onto the stack.
     *
     * A frame with visit = false means that this is the initial
     * visit to 'node', and we should inspect its position and
     * push its children, if appropriate. If the node itself is
     * worth searching, we also push the node again, with visit =
     * true, in between the children.
     *
     * When we encounter a node with visit=true, we actually scan
     * it for matches.
     *
     */

    struct match_frame {
        chunk_file_node *node;
        bool visit;
    };
};

void searcher::find_match(const chunk *chunk,
                          const StringPiece& match,
                          const StringPiece& line) {
    if (!FLAGS_index) {
        find_match_brute(chunk, match, line);
        return;
    }

    run_timer run(git_time_);
    int loff = (unsigned char*)line.data() - chunk->data;

    vector<match_frame> stack;
    assert(chunk->cf_root);
    stack.push_back((match_frame){chunk->cf_root, false});

    debug(kDebugSearch, "find_match(%d)", loff);

    while (!stack.empty() && !exit_reason_) {
        match_frame f = stack.back();
        stack.pop_back();

        chunk_file_node *n = f.node;

        if (f.visit) {
            debug(kDebugSearch, "visit <%d-%d>", n->chunk->left, n->chunk->right);
            assert(loff >= n->chunk->left && loff <= n->chunk->right);
            for (list<indexed_file *>::const_iterator it = n->chunk->files.begin();
                 it != n->chunk->files.end(); ++it) {
                if (!accept(*it))
                    continue;
                if (exit_early())
                    break;
                try_match(line, match, *it);
            }
            continue;
        }

        debug(kDebugSearch,
              "walk <%d-%d> - %d", n->chunk->left, n->chunk->right,
              n->right_limit);

        if (loff > n->right_limit)
            continue;
        if (loff >= n->chunk->left) {
            if (n->right)
                stack.push_back((match_frame){n->right, false});
            if (loff <= n->chunk->right)
                stack.push_back((match_frame){n, true});
        }
        if (n->left)
            stack.push_back((match_frame){n->left, false});
    }
}


void searcher::try_match(const StringPiece& line,
                         const StringPiece& match,
                         indexed_file *sf) {

    int lno = 1;
    auto it = sf->content->begin(cc_->alloc_);

    while (true) {
        for (;it != sf->content->end(cc_->alloc_); ++it) {
            if (line.data() >= it->data() &&
                line.data() <= it->data() + it->size()) {
                lno += count(it->data(), line.data(), '\n');
                break;
            } else {
                lno += count(it->data(), it->data() + it->size(), '\n') + 1;
            }
        }

        debug(kDebugSearch, "found match on %s:%d", sf->path.c_str(), lno);

        if (it == sf->content->end(cc_->alloc_))
            return;

        match_result *m = new match_result;
        m->file = sf;
        m->lno  = lno;
        m->line = line;
        m->matchleft = utf8::distance(line.data(), match.data());
        m->matchright = m->matchleft +
            utf8::distance(match.data(), match.data() + match.size());

        // iterators for forward and backward context
        auto fit = it, bit = it;
        StringPiece l = line;
        int i = 0;

        for (i = 0; i < kContextLines; i++) {
            if (l.data() == bit->data()) {
                if (bit == sf->content->begin(cc_->alloc_))
                    break;
                --bit;
                l = StringPiece(bit->data() + bit->size() + 1, 0);
            }
            l = find_line(*bit, StringPiece(l.data() - 1, 0));
            m->context_before.push_back(l);
        }

        l = line;

        for (i = 0; i < kContextLines; i++) {
            if (l.data() + l.size() == fit->data() + fit->size()) {
                if (++fit == sf->content->end(cc_->alloc_))
                    break;
                l = StringPiece(fit->data() - 1, 0);
            }
            l = find_line(*fit, StringPiece(l.data() + l.size() + 1, 0));
            m->context_after.push_back(l);
        }

        queue_.push(m);
        ++matches_;
        if (exit_early())
            break;

        ++it;
        ++lno;
    }
}

code_searcher::search_thread::search_thread(code_searcher *cs)
    : cs_(cs) {
    if (FLAGS_search) {
        for (int i = 0; i < FLAGS_threads; ++i)
            threads_.push_back(std::move(std::thread(search_one, this)));
    }
}

void code_searcher::search_thread::match_internal(const query &q,
                                                  const code_searcher::search_thread::base_cb& cb,
                                                  match_stats *stats) {
    match_result *m;
    int matches = 0;

    assert(cs_->finalized_);

    if (!FLAGS_search) {
        return;
    }

    if (FLAGS_drop_cache) {
        cs_->alloc_->drop_caches();
    }

    searcher search(cs_, q);
    job j;
    j.search = &search;
    j.pending = 0;

    for (int i = 0; i < FLAGS_threads; ++i) {
        ++j.pending;
        queue_.push(&j);
    }

    for (auto it = cs_->alloc_->begin(); it != cs_->alloc_->end(); it++) {
        j.chunks.push(*it);
    }
    j.chunks.close();

    memset(stats, 0, sizeof *stats);

    while (search.queue_.pop(&m)) {
        matches++;
        cb(m);
        delete m;
    }

    search.get_stats(stats);
    stats->why = search.why();
    stats->matches = matches;
}


code_searcher::search_thread::~search_thread() {
    queue_.close();
    for (auto it = threads_.begin(); it != threads_.end(); ++it)
        it->join();
}

void code_searcher::search_thread::search_one(search_thread *me) {
    job *j;
    while (me->queue_.pop(&j)) {
        chunk *c;
        while (j->chunks.pop(&c)) {
            (*j->search)(c);
        }

        if (--j->pending == 0)
            j->search->queue_.close();
    }
}

void default_re2_options(RE2::Options &opts) {
    opts.set_never_nl(true);
    opts.set_one_line(false);
    opts.set_perl_classes(true);
    opts.set_word_boundary(true);
    opts.set_posix_syntax(true);
    opts.set_word_boundary(true);
    opts.set_log_errors(false);
}
