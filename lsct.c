/*
  Copyright (c) 2015, Maxim Konakov
  All rights reserved.

  Redistribution and use in source and binary forms, with or without modification,
  are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this list
  of conditions and the following disclaimer.
  2. Redistributions in binary form must reproduce the above copyright notice, this list
  of conditions and the following disclaimer in the documentation and/or other materials
  provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
  AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
  OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define _GNU_SOURCE

// The following define is needed for 32bit platforms to avoid
// "Value too large for defined data type" message
// when the scan hits a large (>2Gb) file.
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <ftw.h>
#include <error.h>
#include <malloc.h>
#include <string.h>
#include <magic.h>
#include <errno.h>
#include <search.h>
#include <stdbool.h>

// helpers
#define if_unlikely(x)	if(__builtin_expect(!!(x), 0))

// error handling
#define WARN_ERRNO(err, fmt, ...) error(0, (err), "[WARNING] " fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) WARN_ERRNO(0, fmt, ##__VA_ARGS__)

#define EXIT_ERRNO(err, fmt, ...) error(EXIT_FAILURE, (err), "[ERROR] " fmt, ##__VA_ARGS__)
#define EXIT(fmt, ...) EXIT_ERRNO(0, fmt, ##__VA_ARGS__)

// memory allocation
static
void* check_ptr(void* p)
{
	if_unlikely(!p)
		EXIT_ERRNO(ENOMEM, "");

	return p;
}

#define MEM_ALLOC(n) check_ptr(malloc(n))
#define NEW(T) ((T*)MEM_ALLOC(sizeof(T)))

// settings
static char str_term = '\n';  // -0 / --null
static bool ignore_inaccessible_entries = false;	// -i / --ignore-inaccessible

// printout functions
static
void print_full(const char* name, const char* mime)
{
	printf("%s: %s%c", mime, name, str_term);
}

static
void print_name(const char* name, const char* mime __attribute__((unused)))
{
	printf("%s%c", name, str_term);
}

typedef void (*print_func)(const char*, const char*);

// more settings
static print_func print = print_name;   // -m / --mime
static bool visit_dot_entries = false;  // -a / --all

static
bool is_arg(const char* const param, const char short_arg, const char* const long_arg)
{
	return (param[0] == short_arg && param[1] == 0) || strcmp(param, long_arg) == 0;
}

static
bool is_long_arg(const char* const param, const char* const long_arg)
{
	return strcmp(param, long_arg) == 0;
}

static
int read_switches(const int argc, char** argv)
{
	int i;

	for(i = 1; i < argc && argv[i][0] == '-'; ++i)
	{
		const char* const param = argv[i] + 1;

		if(is_arg(param, 'm', "-mime"))
			print = print_full;
		else if(is_arg(param, '0', "-null"))
			str_term = 0;
		else if(is_arg(param, 'a', "-all"))
			visit_dot_entries = true;
		else if(is_arg(param, 'i', "-ignore-inaccessible"))
			ignore_inaccessible_entries = true;
		else if(is_long_arg(param, "-help"))
		{
			fprintf(stderr,
					"Usage: %s [OPTION]... [FILE]...\n"
					"List FILEs (the current directory by default) recursively, sorted by content-type.\n\n"
					"OPTIONs:\n"
					"  -a, --all      do not ignore entries starting with . (default: off)\n"
					"  -m, --mime     output using the format \"<mime>: <file>\" (default: off)\n"
					"  -0, --null     use null instead of new-line to separate output lines (default: off)\n"
					"  -i, --ignore-inaccessible\n"
					"                 ignore entries that cannot be read (default: off)\n"
					"      --help     display this help and exit.\n",
					program_invocation_short_name);
			exit(EXIT_FAILURE);
		}
		else
			EXIT("Invalid parameter: %s", argv[i]);
	}

	return i;
}

// libmagic
static magic_t libmagic;

static
void free_libmagic()
{
	if(libmagic != NULL)
	{
		magic_close(libmagic);
		libmagic = NULL; // just in case
	}
}

static
void init_libmagic()
{
	libmagic = magic_open(MAGIC_MIME | MAGIC_PRESERVE_ATIME | MAGIC_ERROR);

	if_unlikely(libmagic == NULL)
		EXIT_ERRNO(errno, "Failed to initialise libmagic");

	atexit(free_libmagic);

	if_unlikely(magic_load(libmagic, NULL) == -1)
		EXIT("Failed to load libmagic database: %s", magic_error(libmagic));
}

// data structure for holding entry record
typedef struct entry_record
{
	struct entry_record* next;
	size_t length;
	char name[];
} entry_record;

static
void add_entry_record(entry_record** head, const char* name)
{
	const size_t len = strlen(name);
	entry_record* const p = MEM_ALLOC(sizeof(entry_record) + len + 1);

	p->next = *head;
	*head = p;
	p->length = len;
	memcpy(p->name, name, len + 1);
}

// dictionary record
typedef struct
{
	const char* mime;
	entry_record* list_head;
} dict_item;

// dictionary { mime -> entry_record list }
static void* dict;

static
int cmp_dict_items(const void* s1, const void* s2)
{
	return strcmp(((const dict_item*)s1)->mime, ((const dict_item*)s2)->mime);
}

static
void dict_add(const char* mime, const char* name)
{
	const dict_item key = (dict_item){ mime, NULL };
	dict_item** const ppi = check_ptr(tsearch(&key, &dict, cmp_dict_items));

	if(*ppi == &key) // first item with this mime
	{
		dict_item* const new_item = NEW(dict_item);

		*new_item = (dict_item){ check_ptr(strdup(mime)), NULL };
		*ppi = new_item;
	}

	add_entry_record(&(*ppi)->list_head, name);
}

static
void print_dict_item(const void* s, VISIT value, int level)
{
	if(value == postorder || value == leaf)
	{
		const dict_item* const pi = *(const dict_item**)s;

		for(const entry_record* data = pi->list_head; data; data = data->next)
			print(data->name, pi->mime);
	}
}

#ifdef NO_CLEANUP
#define init_dict() (void)
#else

static
void free_dict_item(void* p)
{
	dict_item* const pi = (dict_item*)p;

	for(entry_record* data = pi->list_head; data; )
	{
		entry_record* const next = data->next;

		free(data);
		data = next;
	}

	free((void*)pi->mime);
	free(pi);
}

static
void free_dict()
{
	tdestroy(dict, free_dict_item);
	dict = NULL; // just in case
}

#define init_dict() atexit(free_dict)
#endif // #ifdef NO_CLEANUP

// callback from nftw() function
static
int visit_file(const char* name, const struct stat* ps, int typeflag, struct FTW* pftw)
{
	// initial selection on type flag
	switch(typeflag)
	{
	case FTW_DNR:
	case FTW_NS:
		WARN("Permission denied: %s", name);
		return FTW_CONTINUE;
	case FTW_D:
	case FTW_F:
	case FTW_SL:
		break;
	default:
		EXIT("Unexpected type flag %d", typeflag);
	}

	const bool skip_entry = !visit_dot_entries && name[pftw->base] == '.';
	const char* mime = NULL;

	// select on mode
	switch(ps->st_mode & S_IFMT)
	{
	case S_IFREG:
		if(skip_entry)
			return FTW_CONTINUE;

		if(ps->st_size > 0)
		{
			mime = magic_file(libmagic, name);

			if_unlikely(!mime)
				EXIT("libmagic error for \"%s\": %s", name, magic_error(libmagic));
		}
		else // optimisation to skip a call to libmagic
			mime = "inode/x-empty; charset=binary";

		break;
	case S_IFLNK:
		if(skip_entry)
			return FTW_CONTINUE;

		// optimisation: emulate libmagic to avoid opening links.
		// libmagic actually returns "inode/symlink; charset=binary"
		// for live links and just "inode/symlink" for broken ones.
		// For our purpose the latter mime type is enough.
		mime = "inode/symlink";
		break;
	case S_IFDIR:
		{
			const char* const base = name + pftw->base;

			// Names like "." or ".." can only come from command line, but we
			// still have to process them here
			if(base[0] == '.' && (base[1] == 0 || (base[1] == '.' && base[2] == 0)))
				return FTW_CONTINUE;
		}
		return skip_entry ? FTW_SKIP_SUBTREE : FTW_CONTINUE;
	default:
		return FTW_CONTINUE;
	}

	dict_add(mime, name);

	return FTW_CONTINUE;
}

static
void scan_dir(const char* dir)
{
	if_unlikely(nftw(dir, visit_file, 20, FTW_PHYS | FTW_ACTIONRETVAL) == -1)
	{
		if(errno == ENOENT && ignore_inaccessible_entries)
			WARN_ERRNO(ENOENT, "\"%s\"", dir);
		else
			EXIT_ERRNO(errno, "\"%s\"", dir);
	}
}

// entry point
int main(int argc, char** argv)
{
	int si = read_switches(argc, argv);

	init_libmagic();
	init_dict();

	if(si == argc)
		scan_dir(".");
	else
		do { scan_dir(argv[si]); } while(++si < argc);

	// print out sorted list
	if_unlikely(!dict)
		EXIT("Nothing to list");

	twalk(dict, print_dict_item);

	return 0;
}
