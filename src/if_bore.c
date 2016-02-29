/* vi:set ts=8 sts=4 sw=4 et:
 *
 * VIM - Vi IMproved    by Bram Moolenaar
 *
 * Bore by Jonas Kjellstrom
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

#include <stdio.h>
#include <string.h>
#include "vim.h"
#include "regexp.h"
#include "if_bore.h"

//#define BORE_VIMPROFILE

#ifdef BORE_VIMPROFILE
#define BORE_VIMPROFILE_INIT proftime_T ptime
#define BORE_VIMPROFILE_START profile_start(&ptime)
#define BORE_VIMPROFILE_STOP(str) do { \
    char mess[100]; \
    profile_end(&ptime); \
    vim_snprintf(mess, 100, "%s %s", profile_msg(&ptime), str); \
    MSG(_(mess)); \
} while(0)
#else
#define BORE_VIMPROFILE_INIT
#define BORE_VIMPROFILE_START
#define BORE_VIMPROFILE_STOP(str)
#endif

#if defined(FEAT_BORE)

#include "roxml.h"
#include <windows.h>

static int bore_canonicalize (const char* src, char* dst, DWORD* attr);
static u32 bore_string_hash(const char* s);
static u32 bore_string_hash_n(const char* s, int n);

void bore_prealloc(bore_alloc_t* p, size_t size)
{
    p->base = (u8*)alloc(size + BORE_CACHELINE);
    p->offset = BORE_CACHELINE - ((size_t)(p->base) & (BORE_CACHELINE - 1));
    p->base += p->offset;
    assert(((size_t)(p->base) & (BORE_CACHELINE - 1)) == 0);
    p->end  = p->base + size;
    p->cursor = p->base;
}

void* bore_alloc(bore_alloc_t* p, size_t size)
{
    void* mem;
    if (p->cursor + size > p->end) {
        // resize
        size_t capacity = p->end - p->base;
        size_t newcapacity = capacity * 2;
        size_t currentsize = p->cursor - p->base;
        size_t newsize = currentsize + size;
        size_t offsetnew;
        u8* basenew;
        if (newsize > newcapacity)
            newcapacity = newsize * 2;
        basenew = alloc(newcapacity + BORE_CACHELINE);
        offsetnew = BORE_CACHELINE - ((size_t)basenew & (BORE_CACHELINE - 1));
        basenew += offsetnew;
        assert(((size_t)basenew & (BORE_CACHELINE - 1)) == 0);
        memcpy(basenew, p->base, currentsize);
        vim_free(p->base - p->offset);
        p->cursor = basenew + currentsize;
        p->base = basenew;
        p->offset = offsetnew;
        p->end = p->base + newcapacity;
    }
    mem = p->cursor;
    p->cursor += size;
    return mem;
}

void bore_alloc_trim(bore_alloc_t* p, size_t size)
{
    p->cursor -= size;
}

void bore_alloc_free(bore_alloc_t* p)
{
    if (p->base)
        vim_free(p->base - p->offset);
}

static bore_t* g_bore = 0;

static void bore_free(bore_t* b)
{
    int i;
    if (!b) return;
    vim_free(b->filelist_tmp_file);
    bore_alloc_free(&b->file_alloc);
    bore_alloc_free(&b->file_proj_alloc);
    bore_alloc_free(&b->file_ext_alloc);
    bore_alloc_free(&b->toggle_index_alloc);
    bore_alloc_free(&b->data_alloc);
    bore_alloc_free(&b->proj_alloc);
    for (i = 0; i < BORE_SEARCH_JOBS; ++i) {
        bore_alloc_free(&b->search[i].filedata);
    }
    vim_free(b);
}

char* bore_str(bore_t* b, u32 offset)
{
    return (char*)(b->data_alloc.base + offset);
}

static u32 bore_strndup(bore_t* b, const char* s, int len)
{
    char* p = (char*)bore_alloc(&b->data_alloc, len + 1);
    memcpy(p, s, len);
    p[len] = 0;
    return p - (char*)b->data_alloc.base;
}

static int bore_is_excluded_file(const char* path)
{
    // TODO-jkjellstrom: Use extension hash when supported?
    const char* ext = (char*)vim_strrchr((char_u*)path, '.');
    if (ext)
    {
        if (0 == STRICMP((char*)ext, ".dll") || 0 == STRICMP((char*)ext, ".vcxproj") || 0 == STRICMP((char*)ext, ".exe"))
            return 1;
    }

    return 0;
}

static void bore_append_vcxproj_files(bore_t* b, int proj_index, node_t** result, int result_count,
        char* filename_buf, char* filename_part, int path_part_len)
{
    int i, file_index;
    bore_file_t* files = (bore_file_t*)bore_alloc(&b->file_alloc, sizeof(bore_file_t)*result_count);

    for(i = 0, file_index = 0; i < result_count; ++i) {
        char buf[BORE_MAX_PATH];
        const char* fn;
        DWORD attr;
        int len;
        int skipFile = 0;

        roxml_get_content(result[i], filename_part, BORE_MAX_PATH - path_part_len, 0);
        len = strlen(filename_part);
        /* roxml sometimes returns paths with trailing " */
        while(len > 0 && filename_part[len - 1] == '\"') {
            --len;
            filename_part[len] = 0;
        }
        fn = (strlen(filename_part) >=2 && filename_part[1] == ':') ? filename_part : filename_buf;

        skipFile = bore_is_excluded_file(fn);
        if (!skipFile && FAIL != bore_canonicalize(fn, buf, &attr)) {
            if (!(FILE_ATTRIBUTE_DIRECTORY & attr)) {
                files[file_index].file = bore_strndup(b, buf, strlen(buf));
                files[file_index].proj_index = proj_index;
                ++file_index;
            }
        }
    }
    b->file_count += file_index;
    bore_alloc_trim(&b->file_alloc, sizeof(bore_file_t)*(result_count - file_index));
}

static void bore_load_vcxproj_filters(bore_t* b, int proj_index, const char* path)
{
    node_t* root;
    node_t** result;
    int result_count;
    char filename_buf[BORE_MAX_PATH];
    char* filename_part;
    int path_part_len;

    root = roxml_load_doc((char*)path);
    if (!root)
        return;

    strcpy(filename_buf, path);
    filename_part = (char*)vim_strrchr((char_u*)filename_buf, '\\') + 1;
    path_part_len = filename_part - filename_buf;

    //result = roxml_xpath(root, "//ClInclude/@Include", &result_count);
    result = roxml_xpath(root, "//@Include", &result_count);
    bore_append_vcxproj_files(b, proj_index, result, result_count, filename_buf, filename_part, path_part_len);
    roxml_release(result);

    roxml_close(root);

}

typedef struct bore_guid_map_t {
    char child[36];
    char parent[36];
} bore_guid_map_t;

static int bore_extract_projects_and_files_from_sln(bore_t* b, const char* sln_path)
{
    regmatch_T regmatch;
    FILE* f;
    char buf[BORE_MAX_PATH];
    char buf2[BORE_MAX_PATH];
    int result = FAIL;
    int state = 0;
    int sln_dir_len = (char*)vim_strrchr((char_u*)sln_path, '\\') - sln_path + 1;

    regmatch.regprog = vim_regcomp((char_u*)"^Project(\"{.\\{-}}\") = \"\\(.\\{-}\\)\", \"\\(.\\{-}\\)\", \"{\\(.\\{-}\\)}\"", RE_MAGIC + RE_STRING);
    regmatch.rm_ic = 0;

    f = fopen(sln_path, "rb");
    if (!f) {
        goto done;
    }
    
    int guid_map_count = 0;
    bore_alloc_t guid_map_alloc;
    bore_prealloc(&guid_map_alloc, 256*(sizeof(bore_guid_map_t)));

    while (0 == vim_fgets((char_u*)buf, sizeof(buf), f)) {
        if (state == 0) {
            if ('\t' == buf[0] || ' ' == buf[0]) {
                if (strstr(buf, "GlobalSection(NestedProjects) = preSolution")) {
                    state = 2;
                }
            }
            else if (vim_regexec_nl(&regmatch, buf, (colnr_T)0)) {
                state = 1;
                bore_proj_t* proj = (bore_proj_t*)bore_alloc(&b->proj_alloc, sizeof(bore_proj_t));
                ++b->proj_count;
                proj->project_sln_name = bore_strndup(b, regmatch.startp[1], regmatch.endp[1] - regmatch.startp[1]);
                proj->project_sln_guid = bore_strndup(b, regmatch.startp[3], regmatch.endp[3] - regmatch.startp[3]);
                proj->project_sln_path = 0;

                if (0 == STRNCMP(regmatch.startp[1], regmatch.startp[2], regmatch.endp[2] - regmatch.startp[2])) {
                    proj->project_file_path = 0; // project is only a solution filter
                }
                else {
                    // TODO-pkack: string hack required since bore_canonicalize use cd which has "\local" stripped
                    vim_strncpy(buf2, (char*)sln_path, sln_dir_len);
                    vim_strncpy(buf2 + sln_dir_len, regmatch.startp[2], regmatch.endp[2] - regmatch.startp[2]);
                    if (FAIL != bore_canonicalize(buf2, buf, 0))
                        proj->project_file_path = bore_strndup(b, buf, strlen(buf));
                    else
                        proj->project_file_path = 0;
                }

                // TODO-pkack: copy and modify string in one step
                // msbuild expects all '.' in project names to be changed to '_'
                char* c = bore_str(b, proj->project_sln_name);
                for (; c && 0 != *c; ++c) {
                    if ('.' == *c)
                        *c = '_';
                }
            }
        }
        else if (state == 1) {
            if (strstr(buf, "ProjectSection(SolutionItems) = preProject")) {
                // skip
            }
            else if (strstr(buf, "EndProjectSection")) {
                state = 0;
            } else {
                char* ends = strstr(buf, " = ");
                if (ends) {
                    DWORD attr = 0;
                    int skipFile = 0;

                    *ends = 0;
                    skipFile = bore_is_excluded_file(buf);
                    if (!skipFile) {
                        // TODO-pkack: string hack required since bore_canonicalize use cd which has "\local" stripped
                        vim_strncpy(buf2, (char*)sln_path, sln_dir_len);
                        strcpy(buf2 + sln_dir_len, &buf[2]);
                        if (FAIL != bore_canonicalize(buf2, buf, &attr)) {
                            if (!(FILE_ATTRIBUTE_DIRECTORY & attr)) {
                                bore_file_t* files = (bore_file_t*)bore_alloc(&b->file_alloc, sizeof(bore_file_t));
                                files->file = bore_strndup(b, buf, strlen(buf));
                                files->proj_index = b->proj_count - 1;
                                ++b->file_count;
                            }
                        }
                    }
                }
                else {
                    state = 0;
                }
            }
        }
        else if (state == 2) {
            if (strstr(buf, "EndGlobalSection")) {
                state = 0;
            }
            else {
                char* ends = strstr(buf, "} = {");
                if (ends) {
                    *ends = 0;
                    int len = strlen(buf + 3); // "\t\t{"
                    bore_guid_map_t* proj_guid = (bore_guid_map_t*)bore_alloc(&guid_map_alloc, sizeof(bore_guid_map_t));
                    ++guid_map_count;
                    memcpy(proj_guid->child, buf + 3, len < 36 ? len : 36);
                    len = strlen(ends + 5);
                    memcpy(proj_guid->parent, ends + 5, len < 36 ? len : 36);
                }
                else {
                    state = 0;
                }
            }
        }
    }

    // TODO-pkack: Optimize this experimental project filter lookup code
    {
        bore_proj_t* proj = (bore_proj_t*)b->proj_alloc.base;
        bore_guid_map_t* guid_map = (bore_guid_map_t*)guid_map_alloc.base;
        int i, j, k;

        for (i = 0; i < b->proj_count; ++i) {
            if (0 == proj[i].project_sln_path) {
                for (j = 0; j < guid_map_count; ++j) {
                    if (0 == STRNCMP(bore_str(b, proj[i].project_sln_guid), guid_map[j].child, 36)) {
                        for (k = 0; k < b->proj_count; ++k) {
                            if (0 == STRNCMP(guid_map[j].parent, bore_str(b, proj[k].project_sln_guid), 36)) {
                                int len = strlen(bore_str(b, proj[k].project_sln_name));
                                vim_strncpy(buf, bore_str(b, proj[k].project_sln_name), len);
                                buf[len] = '\\';
                                strcpy(buf + len + 1, bore_str(b, proj[i].project_sln_name));
                                proj[i].project_sln_name = bore_strndup(b, buf, strlen(buf));
                            }
                        }
                    }
                }
            }
        }
    }

    fclose(f);
    vim_free(regmatch.regprog);
    bore_alloc_free(&guid_map_alloc);

    result = OK;

done:
    return result;
}

static int bore_extract_projects_and_files_from_dir(bore_t* b, const char* sln_path)
{
    BOOL is_git_repo;
    {
        ++emsg_silent;
        char *cmd = "git rev-parse --is-inside-work-tree";
        char* res = get_cmd_output(cmd, NULL, SHELL_READ & SHELL_SILENT, NULL);
        is_git_repo = res && !STRNCMP(res, "true", 4);
        --emsg_silent;
    }
    char* list_cmd = is_git_repo ? "git ls-files" : "dir /s /b /a-d";

    int len;
    char* files = get_cmd_output(list_cmd, 0, SHELL_READ & SHELL_SILENT, &len);
    if (files)
    {
        {
            bore_proj_t* proj = (bore_proj_t*)bore_alloc(&b->proj_alloc, sizeof(bore_proj_t));
            ++b->proj_count;
            proj->project_sln_name = bore_strndup(b, ".\\", 2);
            proj->project_sln_guid = 0;
            proj->project_sln_path = proj->project_sln_name;
            proj->project_file_path = b->sln_path;
        }

        int i;
        int skipFile = 0;
        int sln_dir_len = strlen(sln_path);
        char *cur_dir = "";
        char *start, *end;
        for (i = 0; i < len; ++i)
        {
            start = files + i;
            while (i < len && files[i] != '\r' && files[i] != '\n')
                ++i;
            end = files + i;
            *end = 0;
            if (end[1] == '\n')
                ++i;

            DWORD attr = 0;
            char buf[BORE_MAX_PATH];
            skipFile = bore_is_excluded_file(start);
            if (!skipFile && FAIL != bore_canonicalize(start, buf, &attr)) {
                if (!(FILE_ATTRIBUTE_DIRECTORY & attr)) {

                    // check for new project, i.e. a change in first sublevel directory
                    char* dir_start = buf + sln_dir_len; 
                    char* dir_end = strchr(dir_start, '\\');
                    if (dir_end && STRNCMP(cur_dir, dir_start, dir_end + 1 - dir_start))
                    {
                        bore_proj_t* proj = (bore_proj_t*)bore_alloc(&b->proj_alloc, sizeof(bore_proj_t));
                        ++b->proj_count;
                        proj->project_sln_name = bore_strndup(b, dir_start, dir_end + 1 - dir_start);
                        proj->project_sln_guid = 0;
                        proj->project_sln_path = proj->project_sln_name;
                        proj->project_file_path = bore_strndup(b, buf, dir_end + 1 - buf);

                        cur_dir = bore_str(b, proj->project_sln_name);
                    }

                    bore_file_t* files = (bore_file_t*)bore_alloc(&b->file_alloc, sizeof(bore_file_t));
                    files->file = bore_strndup(b, buf, strlen(buf));
                    files->proj_index = b->proj_count - 1;
                    ++b->file_count;
                }
            }
        }
    }
    return OK;
}

static int bore_extract_files_from_projects(bore_t* b)
{
    bore_proj_t* proj = (bore_proj_t*)b->proj_alloc.base;
    int i;

    for (i = 0; i < b->proj_count; ++i) {
        if (proj[i].project_file_path) {
            //char path[BORE_MAX_PATH];
            //sprintf(path, "%s.filters", bore_str(b, proj[i].project_file_path));
            //bore_load_vcxproj_filters(b, path);
            bore_load_vcxproj_filters(b, i, bore_str(b, proj[i].project_file_path));
        }
    }
    return OK;
}

static int bore_sort_filename(void* ctx, const void* vx, const void* vy)
{
    bore_t* b = (bore_t*)ctx;
    bore_file_t* x = (bore_file_t*)vx;
    bore_file_t* y = (bore_file_t*)vy;
    return STRICMP(bore_str(b, x->file), bore_str(b, y->file));
}

static int bore_find_filename(void* ctx, const void* vx, const void* vy)
{
    bore_t* b = (bore_t*)ctx;
    char* x = (char*)vx;
    bore_file_t* y = (bore_file_t*)vy;
    return STRICMP(x, bore_str(b, y->file));
}

static int bore_sort_project_files(void* ctx, const void* vx, const void* vy)
{
    bore_t* b = (bore_t*)ctx;
    bore_file_t* x = (bore_file_t*)vx;
    bore_file_t* y = (bore_file_t*)vy;
    return x->proj_index - y->proj_index;
}

static int bore_sort_and_cleanup_files(bore_t* b)
{
    bore_file_t* files = (bore_file_t*)b->file_alloc.base;

    if (b->file_count == 0)
        return OK;

    // sort
    qsort_s(files, b->file_count, sizeof(bore_file_t), bore_sort_filename, b);

    // uniq
    {
        bore_file_t* pr = files + 1;
        bore_file_t* pw = files + 1;
        bore_file_t* pend = files + b->file_count;
        int n;
        while(pr < pend) {
            if (0 != STRICMP(bore_str(b, pr->file), bore_str(b, (pr-1)->file))) {
                *pw++ = *pr++;
            } else {
                ++pr;
            }
        }
        n = pw - files;

        // resize file-array
        bore_alloc_trim(&b->file_alloc, sizeof(bore_file_t)*(b->file_count - n));
        b->file_count = n;
    }

    return OK;
}

static int bore_build_project_files(bore_t* b)
{
    bore_file_t* files = (bore_file_t*)b->file_alloc.base;
    bore_alloc(&b->file_proj_alloc, b->file_count * sizeof(bore_file_t));
    bore_file_t* proj_files = (bore_file_t*)b->file_proj_alloc.base;
    memcpy(proj_files, files, b->file_count * sizeof(bore_file_t));
    qsort_s(proj_files, b->file_count, sizeof(bore_file_t), bore_sort_project_files, b);
    return OK;
}

static int bore_build_extension_list(bore_t* b)
{
    bore_file_t* files = (bore_file_t*)b->file_alloc.base;
    bore_alloc(&b->file_ext_alloc, b->file_count * sizeof(u32));
    u32* ext_hash = (u32*)b->file_ext_alloc.base;
    u32 i;
    for (i = 0; i < (u32)b->file_count; ++i) {
        char* path = bore_str(b, files[i].file);
        u32 path_len = (u32)strlen(path);
        char* ext = vim_strrchr(path, '.');

        ext = ext ? ext + 1 : path + path_len;
        ext_hash[i] = bore_string_hash(ext);
    }

    return OK;
}

static int bore_sort_toggle_entry(const void* vx, const void* vy)
{
    const bore_toggle_entry_t* x = (const bore_toggle_entry_t*)vx;
    const bore_toggle_entry_t* y = (const bore_toggle_entry_t*)vy;
    if (x->basename_hash > y->basename_hash)
        return 1;
    else if (x->basename_hash < y->basename_hash)
        return -1;
    else
        return x->extension_index - y->extension_index;
}

static int bore_build_toggle_index(bore_t* b)
{
    bore_file_t* files = (bore_file_t*)b->file_alloc.base;
    u32* file_ext = (u32*)b->file_ext_alloc.base;
    u32 i;
    u32 seq[] = {
        bore_string_hash("cpp"),
        bore_string_hash("cxx"),
        bore_string_hash("c"),
        bore_string_hash("inl"),
        bore_string_hash("hpp"),
        bore_string_hash("hxx"),
        bore_string_hash("h"),
        bore_string_hash("asm"),
        bore_string_hash("s"),
        bore_string_hash("ddf"), 
    };
    bore_prealloc(&b->toggle_index_alloc, b->file_count * sizeof(bore_toggle_entry_t));
    b->toggle_entry_count = 0;
    for (i = 0; i < (u32)b->file_count; ++i) {
        int j;
        int ext_index = -1;
        for (j = 0; j < sizeof(seq)/sizeof(seq[0]); ++j)
            if (seq[j] == file_ext[i]) {
                ext_index = j;
                break;
            }

        if (-1 == ext_index)
            continue;

        char* path = bore_str(b, files[i].file);
        u32 path_len = (u32)strlen(path);
        char* ext = vim_strrchr(path, '.');
        char* basename = vim_strrchr(path, '\\');

        ext = ext ? ext + 1 : path + path_len;
        basename = basename ? basename + 1 : path;

        bore_toggle_entry_t* e = (bore_toggle_entry_t*)bore_alloc(&b->toggle_index_alloc, 
            sizeof(bore_toggle_entry_t));
        e->file = files[i].file;
        e->extension_index = ext_index;
        e->basename_hash = bore_string_hash_n(basename, (int)(ext - basename));
        b->toggle_entry_count++;
    }
    qsort(b->toggle_index_alloc.base, b->toggle_entry_count, sizeof(bore_toggle_entry_t), 
            bore_sort_toggle_entry);

    return OK;
}

static int bore_write_filelist_to_tempfile(bore_t* b)
{
    FILE* f;
    int i;
    char *slndir;
    int slndirlen;
    b->filelist_tmp_file = vim_tempname('b', TRUE);
    if (!b->filelist_tmp_file)
        return FAIL;
    f = fopen(b->filelist_tmp_file, "w");
    if (!f)
        return FAIL;
    slndir = bore_str(b, b->sln_dir);
    slndirlen = strlen(slndir);
    for(i = 0; i < b->file_count; ++i) {
        char *fn = bore_str(b, ((bore_file_t*)b->file_alloc.base)[i].file);
        if (STRNICMP(fn, slndir, slndirlen) == 0)
            fprintf(f, "%s\n", fn + slndirlen);
        else
            fprintf(f, "%s\n", fn);
    }
    fclose(f);
    return OK;
}

static void bore_load_ini(bore_ini_t* ini, const char* dirpath)
{
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);
    ini->cpu_cores = (int)sys_info.dwNumberOfProcessors;
    ini->borebuf_height = 30;
}

static int bore_extract_sln_from_path(bore_t* b, const char* path)
{
    char buf[BORE_MAX_PATH];
    DWORD path_attr = 0;
    if (FAIL == bore_canonicalize((char*)path, buf, &path_attr))
        return FAIL;

    int path_len = strlen(buf);
    b->sln_dir = bore_strndup(b, buf, path_len + 1); // trailing backslash

    if (path_attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        char* sln_dir_str = bore_str(b, b->sln_dir);
        char* pc = vim_strrchr(sln_dir_str, '.');
        // Special case. If the solution path is .git folder, then assume
        // code paths start one level up from that
        if (pc && 0 == STRNCMP(pc, ".git", 4) && (pc[4] == '\\' || pc[4] == 0))
            pc[0] = 0; // Keep trailing backslash
        else
        {
            char* pe = sln_dir_str + path_len - 1;
            if (pe[0] != '\\')
            {
                pe[1] = '\\'; // Add trailing backslash
                pe[2] = 0;
            }
        }
        b->sln_path = b->sln_dir;
    }
    else
    {
        char* sln_dir_str = bore_str(b, b->sln_dir);
        char* pc = vim_strrchr(sln_dir_str, '\\');
        if (pc) {
            pc[1] = 0; // Keep trailing backslash

            // Special case. If the solution file is in a local folder, then assume 
            // code paths start one level up from that
            while (--pc > sln_dir_str) {
                if (*pc == '\\') {
                    if (STRICMP(pc, "\\Local\\") == 0) {
                        pc[1] = 0; // Keep trailing backslash
                    }
                    break;
                }
            }
        }
        b->sln_path = bore_strndup(b, buf, path_len);
    }

    return OK;
}

struct bore_async_execute_context_t
{
    HANDLE wait_thread;
    PROCESS_INFORMATION spawned_process;
    HANDLE result_handle;
};

static struct bore_async_execute_context_t g_bore_async_execute_context;

static void bore_load_sln(const char* path)
{
    g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.result_handle = INVALID_HANDLE_VALUE;

    char buf[BORE_MAX_PATH];
    int i;
    bore_t* b = (bore_t*)alloc(sizeof(bore_t));
    memset(b, 0, sizeof(bore_t));

    bore_free(g_bore);
    g_bore = 0;

    bore_prealloc(&b->data_alloc, 8*1024*1024);
    bore_prealloc(&b->file_alloc, sizeof(bore_file_t)*64*1024);
    bore_prealloc(&b->proj_alloc, sizeof(bore_proj_t)*256);

    for (i = 0; i < BORE_SEARCH_JOBS; ++i) {
        bore_prealloc(&b->search[i].filedata, 1*1024*1024);
    }

    // Allocate something small, so that we can use offset 0 as NULL
    bore_alloc(&b->data_alloc, 1);

    if (FAIL == bore_extract_sln_from_path(b, path))
        goto fail;

    sprintf(buf, "cd %s", bore_str(b, b->sln_dir));
    ++msg_silent;
    do_cmdline_cmd(buf);
    --msg_silent;

    bore_load_ini(&b->ini, bore_str(b, b->sln_dir));

    BORE_VIMPROFILE_INIT;

    if (b->sln_dir != b->sln_path)
    {
        BORE_VIMPROFILE_START;
        if (FAIL == bore_extract_projects_and_files_from_sln(b, bore_str(b, b->sln_path)))
            goto fail;
        BORE_VIMPROFILE_STOP("bore_extract_projects_and_files_from_sln");

        BORE_VIMPROFILE_START;
        if (FAIL == bore_extract_files_from_projects(b))
            goto fail;
        BORE_VIMPROFILE_STOP("bore_extract_files_from_projects");
    }
    else
    {
        BORE_VIMPROFILE_START;
        if (FAIL == bore_extract_projects_and_files_from_dir(b, bore_str(b, b->sln_path)))
            goto fail;
        BORE_VIMPROFILE_STOP("bore_extract_projects_and_files_from_dir");
    }

    BORE_VIMPROFILE_START;
    if (FAIL == bore_sort_and_cleanup_files(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_sort_and_cleanup_files");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_build_project_files(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_build_project_files");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_build_extension_list(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_build_extension_list");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_build_toggle_index(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_build_toggle_index");

    BORE_VIMPROFILE_START;
    if (FAIL == bore_write_filelist_to_tempfile(b))
        goto fail;
    BORE_VIMPROFILE_STOP("bore_write_filelist_to_tempfile");

    sprintf(buf, "let g:bore_base_dir=\'%s\'", bore_str(b, b->sln_dir));
    do_cmdline_cmd(buf);

    do_cmdline_cmd("let g:bore_proj_path=''");

    sprintf(buf, "let g:bore_filelist_file=\'%s\'", b->filelist_tmp_file);
    do_cmdline_cmd(buf);

    g_bore = b;
    return;

fail:
    bore_free(b);
    EMSG2(_("Could not open solution %s"), path);
    return;
}

static void bore_print_sln(DWORD elapsed)
{
    if (g_bore) {
        char status[BORE_MAX_PATH];
        if (elapsed)
        {
            sprintf(status, "%s; %d projects; %d files; %u ms", bore_str(g_bore, g_bore->sln_path),
                    g_bore->proj_count, g_bore->file_count, elapsed);
        }
        else
        {
            sprintf(status, "%s; %d projects; %d files", bore_str(g_bore, g_bore->sln_path),
                    g_bore->proj_count, g_bore->file_count);
        }           
        MSG(_(status));
    }
}

static int bore_canonicalize(const char* src, char* dst, DWORD* attr)
{
    WCHAR wbuf[BORE_MAX_PATH];
    WCHAR wbuf2[BORE_MAX_PATH];
    DWORD fnresult;
    int result = MultiByteToWideChar(CP_UTF8, 0, src, -1, wbuf, BORE_MAX_PATH);
    if (result <= 0) 
        return FAIL;
    fnresult = GetFullPathNameW(wbuf, BORE_MAX_PATH, wbuf2, 0);
    if (!fnresult)
        return FAIL;
    if (attr) {
        *attr = GetFileAttributesW(wbuf);
        if (*attr == INVALID_FILE_ATTRIBUTES)
            return FAIL;
    }
    result = WideCharToMultiByte(CP_UTF8, 0, wbuf2, -1, dst, BORE_MAX_PATH, 0, 0);
    if (!result)
        return FAIL;
    return OK;
}

static u32 bore_string_hash(const char *str)
{
    return bore_string_hash_n(str, -1);
}

static u32 bore_string_hash_n(const char *str, int n)
{
    u32 h;
    u8 *p = (u8*)str;
    u8 *pend = p + n;

    h = 0;
    for (; p != pend && *p != '\0'; p++)
        h = 33 * h + tolower(*p);
    return h + (h >> 5);
}

static void bore_display_search_result(bore_t* b, const char* filename, char* what, int found)
{
    exarg_T eap;
    char* title = (char*)alloc(100);
    vim_snprintf(title, 100, "borefind \"%s\"; %d%s matching lines", what, found > 0 ? found : -found, found < 0 ? " (truncated)" : "");

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cgetfile;
    eap.arg = (char*)filename;
    eap.cmdlinep = &title;
    ex_cfile(&eap);

    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cwindow;
    ex_copen(&eap);

    vim_free(title);
}

static void bore_save_match_to_file(bore_t* b, FILE* cf, const bore_match_t* match, int match_count)
{
    char *slndir = bore_str(b, b->sln_dir);
    int slndirlen = strlen(slndir);
    int i;
    for (i = 0; i < match_count; ++i, ++match) {
        char* fn = bore_str(b, ((bore_file_t*)(b->file_alloc.base))[match->file_index].file);
        if (STRNICMP(fn, slndir, slndirlen) == 0)
            fn += slndirlen;
        fprintf(cf, "%s:%d:%d:%s\n", fn, match->row, match->column + 1, match->line);
    }
}

static int bore_find(bore_t* b, char* what, char* what_ext)
{
    enum { MaxMatch = 1000 };
    bore_file_t* files = (bore_file_t*)b->file_alloc.base;
    int found = 0;
    char_u *tmp = vim_tempname('f', TRUE);
    FILE* cf = 0;
    bore_match_t* match = 0;
    int truncated = 0;

    match = (bore_match_t*)alloc(MaxMatch * sizeof(bore_match_t));

    int threadCount = 4;
    const char_u* threadCountStr = get_var_value((char_u *)"g:bore_search_thread_count");
    if (threadCountStr)
    {
        threadCount = atoi(threadCountStr);
    }

    bore_search_t search;
    search.what = what;
    search.what_len = strlen(what);
    search.ext_count = 0;

    // parse comma separated list of file extensions into list of hashes
    if (what_ext)
    {
        int len = 0;
        char* ext = what_ext;
        char* c;
        for (c = ext; search.ext_count < BORE_MAX_SEARCH_EXTENSIONS; ++c)
        {
            if (*c == ',' || *c == '\0')
            {
                search.ext[search.ext_count++] = bore_string_hash_n(ext, len);
                ext = c + 1;
                len = 0;
            }
            else
            {
                ++len;
            }

            if (*c == '\0')
                break;
        }
    }

    found = bore_dofind(b, threadCount, &truncated, match, MaxMatch, &search);
    if (0 == found)
        goto fail;

    cf = mch_fopen((char *)tmp, "wb");
    if (cf == NULL) {
        EMSG2(_(e_notopen), tmp);
        goto fail;
    }
    bore_save_match_to_file(b, cf, match, found);

    fclose(cf);

    bore_display_search_result(b, tmp, what, truncated ? -found : found);
    mch_remove(tmp);
fail:
    vim_free(tmp);
    vim_free(match);
    if (cf) fclose(cf);
    return truncated ? -found : found;
}

// Display filename in the borebuf.
// Window height is at least minheight (if possible)
// mappings is a null-terminated array of strings with buffer mappings of the form "<key> <command>"
static void bore_show_borebuf(const char* filename, int minheight, const char** mappings)
{
    //char_u    *arg;
    char_u  maparg[512];
    int    n;
#ifdef FEAT_WINDOWS
    win_T    *wp;
#endif
    //    char_u        *p;
    int    empty_fnum = 0;
    int    alt_fnum = 0;
    buf_T    *buf;
    FILE    *filelist_fd = 0;

    if (!g_bore || !g_bore->filelist_tmp_file) {
        EMSG(_("Load a solution first with boresln"));
        return;
    }

#ifdef FEAT_GUI
    need_mouse_correct = TRUE;
#endif

    /*
     * Re-use an existing bore window or open a new one.
     */
    if (!curwin->w_buffer->b_borebuf)
    {
#ifdef FEAT_WINDOWS
        for (wp = firstwin; wp != NULL; wp = wp->w_next)
            if (wp->w_buffer != NULL && wp->w_buffer->b_borebuf)
                break;
        if (wp != NULL && wp->w_buffer->b_nwindows > 0)
            win_enter(wp, TRUE);
        else
#endif
        {
#ifdef FEAT_WINDOWS
            /* Split off help window; put it at far top if no position
             * specified, the current window is vertically split and
             * narrow. */
            n = WSP_HELP;
# ifdef FEAT_VERTSPLIT
            if (cmdmod.split == 0 && curwin->w_width != Columns
                    && curwin->w_width < 80)
                n |= WSP_TOP;
# endif
            if (win_split(0, n) == FAIL)
                goto erret;
#else
            /* use current window */
            if (!can_abandon(curbuf, FALSE))
                goto erret;
#endif

#ifdef FEAT_WINDOWS
            if (curwin->w_height < minheight)
                win_setheight(minheight);
#endif

            alt_fnum = curbuf->b_fnum;
            // Piggyback on the help window which has the properties we want for borebuf too.
            // (readonly, can't insert text, etc)
            (void)do_ecmd(0, (char*)filename, NULL, NULL, ECMD_LASTL, ECMD_HIDE + ECMD_SET_HELP,
#ifdef FEAT_WINDOWS
                    NULL  /* buffer is still open, don't store info */
#else
                    curwin
#endif
                    );

            if (!cmdmod.keepalt)
                curwin->w_alt_fnum = alt_fnum;
            empty_fnum = curbuf->b_fnum;

            // This is the borebuf
            curwin->w_buffer->b_borebuf = 1;
        }
    }

    // Press enter to open the file on the current line
    while(*mappings) {
        sprintf(maparg, "<buffer> %s", *mappings);
        if (0 != do_map(0, maparg, NORMAL, FALSE))
            goto erret;
        ++mappings;
    }

    if (!p_im)
        restart_edit = 0;           /* don't want insert mode in help file */

    /* Delete the empty buffer if we're not using it.  Careful: autocommands
     * may have jumped to another window, check that the buffer is not in a
     * window. */
    if (empty_fnum != 0 && curbuf->b_fnum != empty_fnum)
    {
        buf = buflist_findnr(empty_fnum);
        if (buf != NULL && buf->b_nwindows == 0)
            wipe_buffer(buf, TRUE);
    }

    /* keep the previous alternate file */
    if (alt_fnum != 0 && curwin->w_alt_fnum == empty_fnum && !cmdmod.keepalt)
        curwin->w_alt_fnum = alt_fnum;

    return;
erret:
    EMSG(_("Could not open borebuf"));
}

void bore_async_execute_update(DWORD flags)
{
    DWORD first = flags & (1 << 31);
    DWORD completed = flags & (1 << 30);
    DWORD bytes_read = 0;
    DWORD bytes_avail = 0;
    int errors = 0;
    char buffer[4096];

    BORE_VIMPROFILE_INIT;
    BORE_VIMPROFILE_START;

    for (;;)
    {
        BOOL result = PeekNamedPipe(
            g_bore_async_execute_context.result_handle, 
            NULL,
            0,
            NULL,
            &bytes_avail,
            NULL);

        if (!result) {
            if (!completed)
                EMSG(_("bore_async_execute_update: Failed to peek pipe"));
            break;
        }
        else if (bytes_avail == 0) {
            if (!completed)
                EMSG(_("bore_async_execute_update: No available data in pipe"));
            break;
        }

        result = ReadFile(
            g_bore_async_execute_context.result_handle,
            &buffer[1],
            sizeof(buffer) - 3,
            &bytes_read,
            NULL);

        if (!result) {
            EMSG(_("bore_async_execute_update: Read file error"));
            goto done;
        }
        else if (bytes_read == 0) {
            MSG(_("bore_async_execute_update: No bytes read"));
            goto done;
        }

        // TODO-pkack: Handle partial read lines and bytes_avail > 4093 correctly
        // format as string expression
        buffer[0] = '\'';
        buffer[bytes_read + 1] = '\'';
        buffer[bytes_read + 2] = '\0';

        // TODO-pkack: Is there a way to make cgetexpr/caddexpr handle output the same way as cgetfile/caddfile
        // quick and dirty replace of ' with " instead of escaping correctly
        char* c = buffer + 1;
        char* end = c + bytes_read;
        for (; c != end; ++c)
            if (*c == '\'')
                *c = '\"';

        // add output as error expressions
        char* title = (char*)alloc(100);
        vim_snprintf(title, 100, "borebuild");
        exarg_T eap;
        memset(&eap, 0, sizeof(eap));
        eap.cmdidx = (first && !errors) ? CMD_cgetexpr : CMD_caddexpr;
        eap.cmdlinep = &title;
        eap.arg = buffer;
        ex_cexpr(&eap);
        vim_free(title);

        ++errors;
        if (bytes_read == bytes_avail)
            break;
    }

    if (first && errors) {
        exarg_T eap;
        memset(&eap, 0, sizeof(eap));
        eap.cmdidx = CMD_cwindow;
        ex_copen(&eap);
    }
    update_screen(VALID);
    
done:
    if (completed) {
        CloseHandle(g_bore_async_execute_context.wait_thread);
        CloseHandle(g_bore_async_execute_context.spawned_process.hThread);
        CloseHandle(g_bore_async_execute_context.spawned_process.hProcess);
        CloseHandle(g_bore_async_execute_context.result_handle);
        g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
        g_bore_async_execute_context.spawned_process.hThread = INVALID_HANDLE_VALUE;
        g_bore_async_execute_context.spawned_process.hProcess = INVALID_HANDLE_VALUE;
        g_bore_async_execute_context.result_handle = INVALID_HANDLE_VALUE;
        if (first && !errors)
            MSG(_("Build success"));
        else
            MSG(_("Build done"));
    }

    BORE_VIMPROFILE_STOP("bore_async_update");
}

static DWORD WINAPI bore_async_execute_wait_thread(LPVOID param)
{
    extern HWND s_hwnd;
    DWORD result;
    DWORD bytes_avail;
    DWORD first = 1;
    DWORD completed = 0;

    do {
        result = WaitForSingleObject(g_bore_async_execute_context.spawned_process.hProcess, 1000);

        if (result == WAIT_TIMEOUT)
        {
            BOOL peek_success = PeekNamedPipe(
                g_bore_async_execute_context.result_handle, 
                NULL,
                0,
                0,
                &bytes_avail,
                NULL);

            if (!peek_success || 0 == bytes_avail)
                continue;
        }
        else {
            completed = 1;
        }

        assert(sizeof(WPARAM) == sizeof(&bore_async_execute_update));
        WPARAM wparam = (WPARAM)&bore_async_execute_update;
        LPARAM lparam = (result & 0x3FFFFFFF) | (first << 31) | (completed << 30);
        PostMessage(s_hwnd, WM_USER + 1234, wparam, lparam);

        first = 0;
    } while (completed == 0);

    return 0;
}

static void bore_async_execute(const char* cmdline)
{
    if (g_bore_async_execute_context.wait_thread != INVALID_HANDLE_VALUE) {
        EMSG(_("bore_async_execute: Busy. Cannot launch another process."));
        return;
    }

    autowrite_all();

    exarg_T eap;
    memset(&eap, 0, sizeof(eap));
    eap.cmdidx = CMD_cwindow;
    ex_cclose(&eap);

    HANDLE output_handle;
    HANDLE error_handle;

    SECURITY_ATTRIBUTES sa_attr = {0};
    sa_attr.nLength = sizeof(sa_attr);
    sa_attr.bInheritHandle = TRUE;
    sa_attr.lpSecurityDescriptor = NULL;

    BOOL result = CreatePipe(
        &g_bore_async_execute_context.result_handle,
        &output_handle,
        &sa_attr,
        0);

    if (!result) {
        EMSG(_("bore_async_execute: Failed to create pipe"));
        goto fail;
    }

    result = SetHandleInformation(
        g_bore_async_execute_context.result_handle,
        HANDLE_FLAG_INHERIT,
        0);

    if (!result) {
        EMSG(_("bore_async_execute: Failed to remove inheritable flag for read handle"));
        goto fail;
    }

    result = DuplicateHandle(
        GetCurrentProcess(),
        output_handle,
        GetCurrentProcess(),
        &error_handle,
        0,
        TRUE,
        DUPLICATE_SAME_ACCESS);

    if (!result) {
        EMSG(_("bore_async_execute: Failed to duplicate stdout write handle for stderr"));
        goto fail;
    }

    STARTUPINFO startup_info = {0};
    char cmd[1024];

    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup_info.hStdOutput = output_handle;
    startup_info.hStdError = error_handle;

    if (-1 == _snprintf_s(cmd, sizeof(cmd), sizeof(cmd), "cmd.exe /c \"%s\"", cmdline)) {
        EMSG(_("bore_async_execute: Command line is too long"));
        goto fail;
    }

    memset(
            &g_bore_async_execute_context.spawned_process, 
            0, 
            sizeof(g_bore_async_execute_context.spawned_process));

    BOOL process_created = CreateProcess(
            NULL, 
            cmd, 
            NULL, 
            &sa_attr, 
            TRUE, 
            CREATE_NO_WINDOW, 
            NULL, 
            NULL, 
            &startup_info, 
            &g_bore_async_execute_context.spawned_process);

    if (!process_created) {
        EMSG(_("bore_async_execute: Failed to spawn process"));
        goto fail;
    }

    CloseHandle(output_handle);
    CloseHandle(error_handle);

    DWORD thread_id = 0;

    g_bore_async_execute_context.wait_thread = CreateThread(
            NULL, 
            4096, 
            bore_async_execute_wait_thread, 
            &g_bore_async_execute_context, 
            0, 
            &thread_id);

    if (g_bore_async_execute_context.wait_thread == INVALID_HANDLE_VALUE) {
        EMSG(_("bore_async_execute: Failed to spawn wait thread"));
        goto fail;
    }

    return;

fail:
    CloseHandle(output_handle);
    CloseHandle(error_handle);
    CloseHandle(g_bore_async_execute_context.wait_thread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hThread);
    CloseHandle(g_bore_async_execute_context.spawned_process.hProcess);
    CloseHandle(g_bore_async_execute_context.result_handle);
    g_bore_async_execute_context.wait_thread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.spawned_process.hThread = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.spawned_process.hProcess = INVALID_HANDLE_VALUE;
    g_bore_async_execute_context.result_handle = INVALID_HANDLE_VALUE;
}


#endif

/* Only do the following when the feature is enabled.  Needed for "make
 * depend". */
#if defined(FEAT_BORE) || defined(PROTO)

void ex_boresln(exarg_T *eap)
{
    if (*eap->arg == NUL) {
        bore_print_sln(0);
    } else {
        DWORD start = GetTickCount();
        DWORD elapsed;
        bore_load_sln((char*)eap->arg);
        elapsed = GetTickCount() - start;
        bore_print_sln(elapsed);
    }
}

void borefind_parse_options(char* arg, char** what, char** what_ext)
{
    // Usage: [option(s)] what
    //   -e ext1,ext2,...,ext12
    //      filters the search based on a list of file extensions
    //   - 
    //   -u
    //      an empty (or any unknown) option will force the remainder to be treated as the search string

    char* opt = NUL;
    *what = arg;
    *what_ext = NUL;

    for (; *arg; ++arg)
    {
        if (NUL == opt)
        {
            if ('-' == *arg)
            {
                // found new option marker
                ++arg;
                if (*arg == 'e' && arg[1] == ' ')
                {
                    // found extension option argument start, loop until next space
                    opt = arg;
                    *what_ext = &opt[2];
                    arg += 2;
                }
                else
                {
                    // empty or unknown option, treat the rest as the search string
                    *what = arg + 1;
                    break;
                }
            }
            else
            {
                // no option found, treat the rest as the search string
                *what = arg;
                break;
            }
        }
        else if (' ' == *arg)
        {
            // end current option argument string and search for next option
            opt = NUL;
            *arg = '\0';
        }
    }
}

void ex_borefind(exarg_T *eap)
{
    if (!g_bore) {
        EMSG(_("Load a solution first with boresln"));
    }
    else {
        DWORD start = GetTickCount();
        DWORD elapsed;
        char mess[100];
        char* what;
        char* what_ext;

        borefind_parse_options((char*)eap->arg, &what, &what_ext);
        int found = bore_find(g_bore, what, what_ext);
        elapsed = GetTickCount() - start;
        if (found)
        {
            vim_snprintf(mess, 100, "%d%s matching lines for \"%s\"; %u ms", found > 0 ? found : -found, found < 0 ? " (truncated)" : "", what, elapsed);
            MSG(_(mess));
        }
        else
        {
            vim_snprintf(mess, 100, "No matching lines for \"%s\"; %u ms", what, elapsed);
            EMSG(_(mess));
        }
    }
}

void ex_boreopen(exarg_T *eap)
{
    if (!g_bore)
        EMSG(_("Load a solution first with boresln"));
    else {
        const char* mappings[] = {
            "q <C-w>q<CR>", 
            "<CR> :ZZBoreopenselection<CR>", 
            "<2-LeftMouse> :ZZBoreopenselection<CR>",
            0};
        bore_show_borebuf(g_bore->filelist_tmp_file, g_bore->ini.borebuf_height, mappings);
    }
}

int bore_toggle_entry_score(const char* buffer_name, const char* candidate)
{
    int score = 0;
    while(*buffer_name && *candidate && (tolower(*buffer_name) == tolower(*candidate))) {
        ++buffer_name;
        ++candidate;
        ++score;
    }
    return score;
}

void bore_open_file_buffer(char_u* fn)
{ 
    buf_T* buf;

    buf = buflist_findname_exp(fn);
    if (NULL == buf)
        goto edit;

    if (NULL != buf->b_ml.ml_mfp && NULL != buf_jump_open_tab(buf))
        goto verify;

    set_curbuf(buf, DOBUF_GOTO);

verify:
    if (buf != curbuf)
edit:
    {
        exarg_T ea;
        memset(&ea, 0, sizeof(ea));
        ea.arg = fn;
        ea.cmdidx = CMD_edit;
        do_exedit(&ea, NULL);
    }
}

bore_proj_t* bore_find_project(char* fn)
{
    char path[BORE_MAX_PATH];
    bore_proj_t* projects = (bore_proj_t*)g_bore->proj_alloc.base;

    if (FAIL == bore_canonicalize(fn, path, 0))
        return NULL;

    bore_file_t* file = (bore_file_t*)bsearch_s( 
        path,
        g_bore->file_alloc.base,
        g_bore->file_count,
        sizeof(bore_file_t),
        bore_find_filename,
        g_bore);

    if (NULL == file)
        return NULL;

    return projects + file->proj_index;
}

void ex_boreproj(exarg_T *eap)
{
    if (!g_bore) {
        EMSG(_("Load a solution first with boresln"));
    } else {
        char buf[BORE_MAX_PATH];
        char mess[BORE_MAX_PATH];
        char* arg = (NULL != eap->arg && '\0' != eap->arg[0]) ? eap->arg : curbuf->b_fname;

        bore_proj_t* proj = bore_find_project(arg);
        if (NULL != proj) {
            char *slndir = bore_str(g_bore, g_bore->sln_dir);
            int slndirlen = strlen(slndir);
            char *fn = bore_str(g_bore, proj->project_file_path);
            if (STRNICMP(fn, slndir, slndirlen) == 0)
                fn += slndirlen;

            sprintf(buf, "let g:bore_proj_path=\'%s\'", fn);
            do_cmdline_cmd(buf);
            vim_snprintf(mess, BORE_MAX_PATH, "Project sln path: %s; Project file path: %s",
                bore_str(g_bore, proj->project_sln_name), bore_str(g_bore, proj->project_file_path));
            MSG(_(mess));
        }
        else {
            do_cmdline_cmd("let g:bore_proj_path=''");
            EMSG(_("No project found"));
        }
    }
}

void ex_boretoggle(exarg_T *eap)
{
    if (!g_bore)
        EMSG(_("Load a solution first with boresln"));
    else {
        char path[BORE_MAX_PATH];
        const char* ext;
        const char* basename;
        int path_len;
        u32 basename_hash;
        u32 ext_hash;
        const bore_toggle_entry_t* e_begin = (const bore_toggle_entry_t*)g_bore->toggle_index_alloc.base;
        const bore_toggle_entry_t* e = e_begin;
        const bore_toggle_entry_t* e_end = e + g_bore->toggle_entry_count;
        const bore_toggle_entry_t* e_buf;
        const bore_toggle_entry_t* e_best;
        int e_best_score;

        if (FAIL == bore_canonicalize(curbuf->b_fname, path, 0))
            return;

        path_len = strlen(path);

        ext = vim_strrchr(path, '.');
        ext = ext ? ext + 1 : path + path_len;
        ext_hash = bore_string_hash(ext);

        basename = vim_strrchr(path, '\\');
        basename = basename ? basename + 1 : path;
        basename_hash = bore_string_hash_n(basename, ext - basename);

        // find first entry with identical basename using binary search
        while (e_begin < e_end)
        {
            e = e_begin + ((e_end - e_begin) / 2);
            if (e->basename_hash < basename_hash)
                e_begin = e + 1;
            else
                e_end = e;
        }

        if (e_begin->basename_hash != basename_hash || e_begin != e_end)
            return;

        // set first match and restore e_end
        e = e_begin;
        e_end = (const bore_toggle_entry_t*)g_bore->toggle_index_alloc.base + g_bore->toggle_entry_count;

        // Find the entry of this buffer's file
        for (; e != e_end && e->basename_hash == basename_hash; ++e)
            if (0 == STRICMP(bore_str(g_bore, e->file), path))
                break;

        if (e == e_end || e->basename_hash != basename_hash)
            return;

        // Find what ext to toggle to
        e_buf = e;
        for(;;) {
            ++e;
            if (e == e_end || e->basename_hash != basename_hash)
                e = e_begin;
            if (e == e_buf)
                return; // no match
            if (e->extension_index != e_buf->extension_index) {
                break;
            }
        }

        // Find the best matching ext
        e_best = e;
        e_best_score = bore_toggle_entry_score(path, bore_str(g_bore, e->file));
        ++e;
        for(; e != e_end && e_best->extension_index == e->extension_index; ++e)
        {
            int score = bore_toggle_entry_score(path, bore_str(g_bore, e->file));
            if (score > e_best_score) {
                e_best_score = score;
                e_best = e;
            }
        }

        {
            char *slndir = bore_str(g_bore, g_bore->sln_dir);
            int slndirlen = strlen(slndir);
            char *fn = bore_str(g_bore, e_best->file);
            if (STRNICMP(fn, slndir, slndirlen) == 0)
                fn += slndirlen;
            bore_open_file_buffer(fn);
        }
    }
}

void ex_borebuild(exarg_T *eap)
{
    if (!g_bore) {
        EMSG(_("Load a solution first with boresln"));
    } else if (!mch_can_exe("msbuild", NULL, TRUE)) {
        EMSG(_("Failed to find msbuild executable in path"));
    }
    else {
        char cmd[1024];
        bore_proj_t* proj = NULL;
        char* src_file = NULL;
        char* platform = strstr(bore_str(g_bore, g_bore->sln_path), "vim_vs2013") != 0 ? "Win32" : "x64";
        char* configuration = "Release";
        char* slndir = bore_str(g_bore, g_bore->sln_dir);
        int slndirlen = strlen(slndir);

        if (eap->cmdidx == CMD_borebuildfile) {
            // Specififed source file argument
            if (NULL != eap->arg && '\0' != eap->arg[0]) {
                src_file = eap->arg;
                proj = bore_find_project(src_file);
                if (NULL == proj) {
                    EMSG(_("borebuildfile: Failed to lookup specified file"));
                    return;
                }
            }
            // Current buffer
            else if (NULL != curbuf->b_fname && '\0' != curbuf->b_fname) {
                src_file = curbuf->b_fname;
                proj = bore_find_project(src_file);
                if (NULL == proj) {
                    EMSG(_("borebuildfile: Failed to lookup current bufffer"));
                    return;
                }
            }
            else {
                EMSG(_("borebuildfile: No file specified, and no current buffer"));
                return;
            }
        }
        else if (eap->cmdidx == CMD_borebuildproj) {
            // Specififed project name argument
            if (NULL != eap->arg && '\0' != eap->arg[0]) {
                bore_proj_t* projects = (bore_proj_t*)g_bore->proj_alloc.base;
                int i;
                for (i = 0; i < g_bore->proj_count; ++i) {
                    if (0 == STRICMP(eap->arg, bore_str(g_bore, projects[i].project_sln_name))) {
                        proj = &projects[i];
                        break;
                    }
                }
                if (NULL == proj) {
                    EMSG(_("borebuildproj: Failed to lookup specified project"));
                    return;
                }
            }
            // Current buffer
            else if (NULL != curbuf->b_fname && '\0' != curbuf->b_fname) {
                proj = bore_find_project(curbuf->b_fname);
                if (NULL == proj) {
                    EMSG(_("borebuildproj: Failed to lookup current buffer"));
                    return;
                }
            }
            else {
                EMSG(_("borebuildproj: No project specified, and no current buffer"));
                return;
            }
            // TODO-pkack: Can target Build/Rebuild/Clean be specified for a specific project?
            // The following MSDN example does not work
            // https://msdn.microsoft.com/en-us/library/ms171486.aspx
            // msbuild SlnFolders.sln /t:NotInSlnfolder:Rebuild;NewFolder\InSolutionFolder:Clean 
            // Note: '.' in project names must be replaced with '_' for msbuild targets
        }

        if (eap->cmdidx == CMD_borebuildfile) {
            char* proj_file = bore_str(g_bore, proj->project_file_path);
            if (STRNICMP(proj_file, slndir, slndirlen) == 0)
                proj_file += slndirlen;

            if (STRNICMP(src_file, slndir, slndirlen) == 0)
                src_file += slndirlen;

            vim_snprintf(cmd, 1024,
                "msbuild %s /t:ClCompile /p:SelectedFiles=\"%s\" /p:Platform=%s /p:Configuration=%s " \
                "/v:q /nologo /fl /flp:ShowTimestamp;verbosity=normal",
                proj_file, src_file, platform, configuration);
        }
        else {
            char* sln_file = bore_str(g_bore, g_bore->sln_path) + slndirlen;
            char* target;
            if (eap->cmdidx == CMD_borebuildproj)
                target = bore_str(g_bore, proj->project_sln_name);
            else
                target = eap->forceit ? "Rebuild" : "Build";

            vim_snprintf(cmd, 1024,
                "msbuild %s /t:%s /p:Platform=%s /p:Configuration=%s " \
                "/v:q /nologo /fl /flp:ShowTimestamp;verbosity=normal " \
                "/p:BuildProjectReferences=true " \
                "/m:%d /p:MultiProcessorCompilation=true;CL_MPCount=%d",
                sln_file, target, platform, configuration,
                g_bore->ini.cpu_cores, g_bore->ini.cpu_cores);
        }

        bore_async_execute(cmd);
        char* c = strstr(cmd, " /v:");
        if (c)
            *c = '\0';
        MSG(_(cmd));
    }
}


// Internal functions

// Open the file on the current row in the current buffer 
void ex_Boreopenselection(exarg_T *eap)
{
    char_u* fn;
    if (!g_bore)
        return;
    fn = vim_strsave(ml_get_curline());
    if (!fn)
        return;

    win_close(curwin, TRUE);

    bore_open_file_buffer(fn);
    vim_free(fn);
}

#endif
