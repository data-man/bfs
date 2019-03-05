/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2015-2019 Tavian Barnes <tavianator@tavianator.com>        *
 *                                                                          *
 * Permission to use, copy, modify, and/or distribute this software for any *
 * purpose with or without fee is hereby granted.                           *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES *
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         *
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  *
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   *
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    *
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  *
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           *
 ****************************************************************************/

#include "color.h"
#include "bftw.h"
#include "posix1e.h"
#include "stat.h"
#include "trie.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * The parsed form of LS_COLORS.
 */
struct colors {
	const char *reset;
	const char *bold;
	const char *gray;
	const char *red;
	const char *green;
	const char *yellow;
	const char *blue;
	const char *magenta;
	const char *cyan;

	const char *normal;
	const char *file;
	const char *dir;
	const char *link;
	const char *multi_hard;
	const char *pipe;
	const char *door;
	const char *block;
	const char *chardev;
	const char *orphan;
	const char *missing;
	const char *socket;
	const char *setuid;
	const char *setgid;
	const char *capable;
	const char *sticky_ow;
	const char *ow;
	const char *sticky;
	const char *exec;

	const char *warning;
	const char *error;

	struct trie ext_colors;

	char *data;
};

/**
 * A named color for parsing and lookup.
 */
struct color_name {
	const char *name;
	size_t offset;
};

#define COLOR_NAME(name, field) {name, offsetof(struct colors, field)}

/**
 * All the known color names that can be referenced in LS_COLORS.
 */
static const struct color_name color_names[] = {
	COLOR_NAME("bd", block),
	COLOR_NAME("bld", bold),
	COLOR_NAME("blu", blue),
	COLOR_NAME("ca", capable),
	COLOR_NAME("cd", chardev),
	COLOR_NAME("cyn", cyan),
	COLOR_NAME("di", dir),
	COLOR_NAME("do", door),
	COLOR_NAME("er", error),
	COLOR_NAME("ex", exec),
	COLOR_NAME("fi", file),
	COLOR_NAME("grn", green),
	COLOR_NAME("gry", gray),
	COLOR_NAME("ln", link),
	COLOR_NAME("mag", magenta),
	COLOR_NAME("mh", multi_hard),
	COLOR_NAME("mi", missing),
	COLOR_NAME("no", normal),
	COLOR_NAME("or", orphan),
	COLOR_NAME("ow", ow),
	COLOR_NAME("pi", pipe),
	COLOR_NAME("red", red),
	COLOR_NAME("rs", reset),
	COLOR_NAME("sg", setgid),
	COLOR_NAME("so", socket),
	COLOR_NAME("st", sticky),
	COLOR_NAME("su", setuid),
	COLOR_NAME("tw", sticky_ow),
	COLOR_NAME("wr", warning),
	COLOR_NAME("ylw", yellow),
	{0},
};

/** Get a color from the table. */
static const char **get_color(const struct colors *colors, const char *name) {
	for (const struct color_name *entry = color_names; entry->name; ++entry) {
		if (strcmp(name, entry->name) == 0) {
			return (const char **)((char *)colors + entry->offset);
		}
	}

	return NULL;
}

/** Set the value of a color. */
static void set_color(struct colors *colors, const char *name, const char *value) {
	const char **color = get_color(colors, name);
	if (color) {
		*color = value;
	}
}

/**
 * Transform a file extension for fast lookups, by reversing and lowercasing it.
 */
static char *extxfrm(const char *ext) {
	size_t len = strlen(ext);
	char *ret = malloc(len + 1);
	if (!ret) {
		return NULL;
	}

	for (size_t i = 0; i < len; ++i) {
		char c = ext[len - i - 1];

		// What's internationalization?  Doesn't matter, this is what
		// GNU ls does.  Luckily, since there's no standard C way to
		// casefold.  Not using tolower() here since it respects the
		// current locale.
		if (c >= 'A' && c <= 'Z') {
			c += 'a' - 'A';
		}

		ret[i] = c;
	}

	ret[len] = '\0';
	return ret;
}

/**
 * Set the color for an extension.
 */
static int set_ext_color(struct colors *colors, const char *key, const char *value) {
	char *xfrm = extxfrm(key);
	if (!xfrm) {
		return -1;
	}

	while (true) {
		// A later *.x should override any earlier *.x, *.y.x, etc.
		struct trie_leaf *match = trie_find_postfix(&colors->ext_colors, xfrm);
		if (!match) {
			break;
		}

		trie_remove_mem(&colors->ext_colors, match->key, match->length);
	}

	struct trie_leaf *leaf = trie_insert_str(&colors->ext_colors, xfrm);
	free(xfrm);
	if (leaf) {
		leaf->value = value;
		return 0;
	} else {
		return -1;
	}
}

/**
 * Find a color by an extension.
 */
static const char *get_ext_color(const struct colors *colors, const char *filename) {
	char *xfrm = extxfrm(filename);
	if (!xfrm) {
		return NULL;
	}

	const struct trie_leaf *leaf = trie_find_prefix(&colors->ext_colors, xfrm);
	free(xfrm);
	if (leaf) {
		return leaf->value;
	} else {
		return NULL;
	}
}

struct colors *parse_colors(const char *ls_colors) {
	struct colors *colors = malloc(sizeof(struct colors));
	if (!colors) {
		goto done;
	}

	// From man console_codes
	colors->reset   = "0";
	colors->bold    = "01";
	colors->gray    = "01;30";
	colors->red     = "01;31";
	colors->green   = "01;32";
	colors->yellow  = "01;33";
	colors->blue    = "01;34";
	colors->magenta = "01;35";
	colors->cyan    = "01;36";

	// Defaults generated by dircolors --print-database
	colors->normal     = NULL;
	colors->file       = NULL;
	colors->dir        = "01;34";
	colors->link       = "01;36";
	colors->multi_hard = NULL;
	colors->pipe       = "40;33";
	colors->socket     = "01;35";
	colors->door       = "01;35";
	colors->block      = "40;33;01";
	colors->chardev    = "40;33;01";
	colors->orphan     = "40;31;01";
	colors->setuid     = "37;41";
	colors->setgid     = "30;43";
	colors->capable    = "30;41";
	colors->sticky_ow  = "30;42";
	colors->ow         = "34;42";
	colors->sticky     = "37;44";
	colors->exec       = "01;32";
	colors->warning    = "40;33;01";
	colors->error      = "40;31;01";
	colors->data       = NULL;

	trie_init(&colors->ext_colors);

	if (ls_colors) {
		colors->data = strdup(ls_colors);
	}

	if (!colors->data) {
		goto done;
	}

	for (char *chunk = colors->data, *next; chunk; chunk = next) {
		next = strchr(chunk, ':');
		if (next) {
			*next++ = '\0';
		}

		char *equals = strchr(chunk, '=');
		if (!equals) {
			continue;
		}
		*equals = '\0';

		const char *key = chunk;
		const char *value = equals + 1;

		if (key[0] == '*') {
			set_ext_color(colors, key + 1, value);
		} else {
			// All-zero values should be treated like NULL, to fall
			// back on any other relevant coloring for that file
			if (strcmp(key, "rs") != 0 && strspn(value, "0") == strlen(value)) {
				value = NULL;
			}
			set_color(colors, key, value);
		}
	}

done:
	return colors;
}

void free_colors(struct colors *colors) {
	if (colors) {
		trie_destroy(&colors->ext_colors);
		free(colors->data);
		free(colors);
	}
}

CFILE *cfopen(const char *path, const struct colors *colors) {
	CFILE *cfile = malloc(sizeof(*cfile));
	if (!cfile) {
		return NULL;
	}

	cfile->close = false;
	cfile->file = fopen(path, "wb");
	if (!cfile->file) {
		cfclose(cfile);
		return NULL;
	}
	cfile->close = true;

	if (isatty(fileno(cfile->file))) {
		cfile->colors = colors;
	} else {
		cfile->colors = NULL;
	}

	return cfile;
}

CFILE *cfdup(FILE *file, const struct colors *colors) {
	CFILE *cfile = malloc(sizeof(*cfile));
	if (!cfile) {
		return NULL;
	}

	cfile->close = false;
	cfile->file = file;

	if (isatty(fileno(file))) {
		cfile->colors = colors;
	} else {
		cfile->colors = NULL;
	}

	return cfile;
}

int cfclose(CFILE *cfile) {
	int ret = 0;
	if (cfile) {
		if (cfile->close) {
			ret = fclose(cfile->file);
		}
		free(cfile);
	}
	return ret;
}

/** Get the color for a file. */
static const char *file_color(const struct colors *colors, const char *filename, const struct BFTW *ftwbuf) {
	const struct bfs_stat *sb = ftwbuf->statbuf;
	if (!sb) {
		if (colors->missing) {
			return colors->missing;
		} else {
			return colors->orphan;
		}
	}

	const char *color = NULL;

	switch (sb->mode & S_IFMT) {
	case S_IFREG:
		if (sb->mode & S_ISUID) {
			color = colors->setuid;
		} else if (sb->mode & S_ISGID) {
			color = colors->setgid;
		} else if (colors->capable && bfs_check_capabilities(ftwbuf)) {
			color = colors->capable;
		} else if (sb->mode & 0111) {
			color = colors->exec;
		}

		if (!color && sb->nlink > 1) {
			color = colors->multi_hard;
		}

		if (!color) {
			color = get_ext_color(colors, filename);
		}

		if (!color) {
			color = colors->file;
		}

		break;

	case S_IFDIR:
		if (sb->mode & S_ISVTX) {
			if (sb->mode & S_IWOTH) {
				color = colors->sticky_ow;
			} else {
				color = colors->sticky;
			}
		} else if (sb->mode & S_IWOTH) {
			color = colors->ow;
		}

		if (!color) {
			color = colors->dir;
		}

		break;

	case S_IFLNK:
		color = colors->link;
		if (colors->orphan && xfaccessat(ftwbuf->at_fd, ftwbuf->at_path, F_OK) != 0) {
			color = colors->orphan;
		}
		break;

	case S_IFBLK:
		color = colors->block;
		break;
	case S_IFCHR:
		color = colors->chardev;
		break;
	case S_IFIFO:
		color = colors->pipe;
		break;
	case S_IFSOCK:
		color = colors->socket;
		break;

#ifdef S_IFDOOR
	case S_IFDOOR:
		color = colors->door;
		break;
#endif
	}

	if (!color) {
		color = colors->normal;
	}

	return color;
}

/** Print an ANSI escape sequence. */
static int print_esc(const char *esc, FILE *file) {
	if (fputs("\033[", file) == EOF) {
		return -1;
	}
	if (fputs(esc, file) == EOF) {
		return -1;
	}
	if (fputs("m", file) == EOF) {
		return -1;
	}

	return 0;
}

/** Print a string with an optional color. */
static int print_colored(const struct colors *colors, const char *esc, const char *str, size_t len, FILE *file) {
	if (esc) {
		if (print_esc(esc, file) != 0) {
			return -1;
		}
	}
	if (fwrite(str, 1, len, file) != len) {
		return -1;
	}
	if (esc) {
		if (print_esc(colors->reset, file) != 0) {
			return -1;
		}
	}

	return 0;
}

/** Print the path to a file with the appropriate colors. */
static int print_path(CFILE *cfile, const struct BFTW *ftwbuf) {
	const struct colors *colors = cfile->colors;
	FILE *file = cfile->file;
	const char *path = ftwbuf->path;

	if (!colors) {
		return fputs(path, file) == EOF ? -1 : 0;
	}

	if (ftwbuf->nameoff > 0) {
		if (print_colored(colors, colors->dir, path, ftwbuf->nameoff, file) != 0) {
			return -1;
		}
	}

	const char *filename = path + ftwbuf->nameoff;
	const char *color = file_color(colors, filename, ftwbuf);
	if (print_colored(colors, color, filename, strlen(filename), file) != 0) {
		return -1;
	}

	return 0;
}

/** Print a link target with the appropriate colors. */
static int print_link(CFILE *cfile, const struct BFTW *ftwbuf) {
	int ret = -1;

	char *target = xreadlinkat(ftwbuf->at_fd, ftwbuf->at_path, 0);
	if (!target) {
		goto done;
	}

	struct BFTW altbuf = *ftwbuf;
	altbuf.path = target;
	altbuf.nameoff = xbasename(target) - target;

	struct bfs_stat statbuf;
	if (bfs_stat(ftwbuf->at_fd, ftwbuf->at_path, 0, 0, &statbuf) == 0) {
		altbuf.statbuf = &statbuf;
	} else {
		altbuf.statbuf = NULL;
	}

	ret = print_path(cfile, &altbuf);

done:
	free(target);
	return ret;
}

int cfprintf(CFILE *cfile, const char *format, ...) {
	va_list args;
	va_start(args, format);
	int ret = cvfprintf(cfile, format, args);
	va_end(args);
	return ret;
}

int cvfprintf(CFILE *cfile, const char *format, va_list args) {
	const struct colors *colors = cfile->colors;
	FILE *file = cfile->file;
	int error = errno;

	for (const char *i = format; *i; ++i) {
		size_t verbatim = strcspn(i, "%$");
		if (fwrite(i, 1, verbatim, file) != verbatim) {
			return -1;
		}

		i += verbatim;
		switch (*i) {
		case '%':
			switch (*++i) {
			case '%':
				if (fputc('%', file) == EOF) {
					return -1;
				}
				break;

			case 'c':
				if (fputc(va_arg(args, int), file) == EOF) {
					return -1;
				}
				break;

			case 'd':
				if (fprintf(file, "%d", va_arg(args, int)) < 0) {
					return -1;
				}
				break;

			case 'g':
				if (fprintf(file, "%g", va_arg(args, double)) < 0) {
					return -1;
				}
				break;

			case 's':
				if (fputs(va_arg(args, const char *), file) == EOF) {
					return -1;
				}
				break;

			case 'z':
				++i;
				if (*i != 'u') {
					goto invalid;
				}
				if (fprintf(file, "%zu", va_arg(args, size_t)) < 0) {
					return -1;
				}
				break;

			case 'm':
				if (fputs(strerror(error), file) == EOF) {
					return -1;
				}
				break;

			case 'p':
				switch (*++i) {
				case 'P':
					if (print_path(cfile, va_arg(args, const struct BFTW *)) != 0) {
						return -1;
					}
					break;

				case 'L':
					if (print_link(cfile, va_arg(args, const struct BFTW *)) != 0) {
						return -1;
					}
					break;

				default:
					goto invalid;
				}

				break;

			default:
				goto invalid;
			}
			break;

		case '$':
			switch (*++i) {
			case '$':
				if (fputc('$', file) == EOF) {
					return -1;
				}
				break;

			case '{': {
				++i;
				const char *end = strchr(i, '}');
				if (!end) {
					goto invalid;
				}
				if (!colors) {
					i = end;
					break;
				}

				size_t len = end - i;
				char name[len + 1];
				memcpy(name, i, len);
				name[len] = '\0';

				const char **esc = get_color(colors, name);
				if (!esc) {
					goto invalid;
				}
				if (*esc) {
					if (print_esc(*esc, file) != 0) {
						return -1;
					}
				}

				i = end;
				break;
			}

			default:
				goto invalid;
			}
			break;

		default:
			return 0;
		}

	}

	return 0;

invalid:
	assert(false);
	errno = EINVAL;
	return -1;
}
