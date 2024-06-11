/* ----------------------------------------------------------------------- *
 *
 *   Copyright 2001-2024 H. Peter Anvin - All Rights Reserved
 *
 *   This program is free software available under the same license
 *   as the "OpenBSD" operating system, distributed at
 *   http://www.openbsd.org/.
 *
 * ----------------------------------------------------------------------- */

/*
 * remap.c
 *
 * Perform regular-expression based filename remapping.
 */

#include "config.h"             /* Must be included first! */
#include <ctype.h>
#include <syslog.h>
#include <regex.h>

#include "tftpd.h"
#include "remap.h"

#define DEADMAN_MAX_STEPS	4096    /* Timeout after this many steps */
#define MAXLINE			16384   /* Truncate a line at this many bytes */

#define RULE_REWRITE	0x01    /* This is a rewrite rule */
#define RULE_GLOBAL	0x02    /* Global rule (repeat until no match) */
#define RULE_EXIT	0x04    /* Exit after matching this rule */
#define RULE_RESTART	0x08    /* Restart at the top after matching this rule */
#define RULE_ABORT	0x10    /* Terminate processing with an error */
#define RULE_INVERSE	0x20    /* Execute if regex *doesn't* match */
#define RULE_IPV4	0x40	/* IPv4 only */
#define RULE_IPV6	0x80	/* IPv6 only */

#define RULE_HASFILE	0x100	/* Valid if rule results in a valid filename */
#define RULE_RRQ	0x200	/* Get (read) only */
#define RULE_WRQ	0x400	/* Put (write) only */
#define RULE_SEDG	0x800   /* sed-style global */

int deadman_max_steps = DEADMAN_MAX_STEPS;

struct rule {
    struct rule *next;
    int nrule;
    unsigned int rule_flags;
    regex_t rx;
    const char *pattern;
};

static int xform_null(int c)
{
    return c;
}

static int xform_toupper(int c)
{
    return toupper(c);
}

static int xform_tolower(int c)
{
    return tolower(c);
}

/*
 * Do \-substitution.  Call with string == NULL to get length only.
 * "start" indicates an offset into the input buffer where the pattern
 * match was started.
 */
static int do_genmatchstring(char *string, const char *pattern,
                             const char *ibuf,
                             const regmatch_t * pmatch,
                             match_pattern_callback macrosub,
                             int start, int *nextp)
{
    int (*xform) (int) = xform_null;
    int len = 0;
    int n, mlen, sublen;
    int endbytes;
    const char *input = ibuf + start;

    /* Get section before match; note pmatch[0] is the whole match */
    endbytes = strlen(input) - pmatch[0].rm_eo;
    len = start + pmatch[0].rm_so;
    if (string) {
        /* Copy the prefix before "start" as well! */
        memcpy(string, ibuf, start + pmatch[0].rm_so);
        string += start + pmatch[0].rm_so;
    }

    /* Transform matched section */
    while (*pattern) {
        mlen = 0;

        if (*pattern == '\\' && pattern[1] != '\0') {
            char macro = pattern[1];
            switch (macro) {
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                n = pattern[1] - '0';

                if (pmatch[n].rm_so != -1) {
                    mlen = pmatch[n].rm_eo - pmatch[n].rm_so;
                    len += mlen;
                    if (string) {
                        const char *p = input + start + pmatch[n].rm_so;
                        while (mlen--)
                            *string++ = xform(*p++);
                    }
                }
                break;

            case 'L':
                xform = xform_tolower;
                break;

            case 'U':
                xform = xform_toupper;
                break;

            case 'E':
                xform = xform_null;
                break;

            default:
                if (macrosub && (sublen = macrosub(macro, string)) >= 0) {
                    while (sublen--) {
                        len++;
                        if (string) {
                            *string = xform(*string);
                            string++;
                        }
                    }
                } else {
                    len++;
                    if (string)
                        *string++ = xform(pattern[1]);
                }
            }
            pattern += 2;
        } else {
            len++;
            if (string)
                *string++ = xform(*pattern);
            pattern++;
        }
    }

    /* Pointer to post-substitution tail */
    if (nextp)
        *nextp = len;

    /* Copy section after match */
    len += endbytes;
    if (string) {
        memcpy(string, input + pmatch[0].rm_eo, endbytes);
        string[endbytes] = '\0';
    }

    return len;
}

/*
 * Ditto, but allocate the string in a new buffer
 */

static int genmatchstring(char **string, const char *pattern,
                          const char *ibuf,
                          const regmatch_t * pmatch,
                          match_pattern_callback macrosub,
                          int start, int *nextp)
{
    int len;
    char *buf;

    len = do_genmatchstring(NULL, pattern, ibuf, pmatch,
                            macrosub, start, NULL);
    *string = buf = tfmalloc(len + 1);
    return do_genmatchstring(buf, pattern, ibuf, pmatch,
                             macrosub, start, nextp);
}

/*
 * Extract a string terminated by non-escaped whitespace; ignoring
 * leading whitespace.  Consider an unescaped # to be a comment marker,
 * functionally \n.
 */
static int readescstring(char *buf, char **str)
{
    char *p = *str;
    int wasbs = 0, len = 0;

    while (*p && isspace(*p))
        p++;

    if (!*p) {
        *buf = '\0';
        *str = p;
        return 0;
    }

    while (*p) {
        if (!wasbs && (isspace(*p) || *p == '#')) {
            *buf = '\0';
            *str = p;
            return len;
        }
        /* Important: two backslashes leave us in the !wasbs state! */
        wasbs = !wasbs && (*p == '\\');
        *buf++ = *p++;
        len++;
    }

    *buf = '\0';
    *str = p;
    return len;
}

/* Parse a line into a set of instructions */
static int parseline(char *line, struct rule *r, int lineno)
{
    char buffer[MAXLINE];
    char *p;
    int rv;
    int rxflags = REG_EXTENDED;
    static int nrule;

    memset(r, 0, sizeof *r);
    r->nrule = nrule;

    if (!readescstring(buffer, &line))
        return 0;               /* No rule found */

    for (p = buffer; *p; p++) {
        switch (*p) {
        case 'r':
            r->rule_flags |= RULE_REWRITE;
            break;
        case 'g':
            if (r->rule_flags & RULE_GLOBAL)
                r->rule_flags |= RULE_SEDG;
            else
                r->rule_flags |= RULE_GLOBAL;
            break;
        case 'e':
            r->rule_flags |= RULE_EXIT;
            break;
        case 'E':
            r->rule_flags |= RULE_HASFILE;
            break;
        case 's':
            r->rule_flags |= RULE_RESTART;
            break;
        case 'a':
            r->rule_flags |= RULE_ABORT;
            break;
        case 'i':
            rxflags |= REG_ICASE;
            break;
        case '~':
            r->rule_flags |= RULE_INVERSE;
            break;
        case '4':
            r->rule_flags |= RULE_IPV4;
            break;
        case '6':
            r->rule_flags |= RULE_IPV6;
            break;
        case 'G':
            r->rule_flags |= RULE_RRQ;
            break;
        case 'P':
            r->rule_flags |= RULE_WRQ;
            break;
        default:
            syslog(LOG_ERR,
                   "Remap command \"%s\" on line %d contains invalid char \"%c\"",
                   buffer, lineno, *p);
            return -1;          /* Error */
            break;
        }
    }

    if (r->rule_flags & RULE_REWRITE) {
        if (r->rule_flags & RULE_INVERSE) {
            syslog(LOG_ERR, "r rules cannot be inverted, line %d: %s\n",
                   lineno, line);
            return -1;              /* Error */
        }
        if ((r->rule_flags & (RULE_GLOBAL|RULE_SEDG|RULE_HASFILE))
            == (RULE_GLOBAL|RULE_HASFILE)) {
            syslog(LOG_ERR, "E rules cannot be combined with g (but gg is OK), line %d: %s\n",
                   lineno, line);
            return -1;              /* Error */
        }
    } else {
        /* RULE_GLOBAL and RULE_SEDG are meaningless without RULE_REWRITE */
        r->rule_flags &= ~(RULE_GLOBAL|RULE_SEDG);
    }

    /* Read and compile the regex */
    if (!readescstring(buffer, &line)) {
        syslog(LOG_ERR, "No regex on remap line %d: %s\n", lineno, line);
        return -1;              /* Error */
    }

    if ((rv = regcomp(&r->rx, buffer, rxflags)) != 0) {
        char errbuf[BUFSIZ];
        regerror(rv, &r->rx, errbuf, BUFSIZ);
        syslog(LOG_ERR, "Bad regex in remap line %d: %s\n", lineno,
               errbuf);
        return -1;              /* Error */
    }

    /* Read the rewrite pattern, if any */
    if (readescstring(buffer, &line)) {
        r->pattern = tfstrdup(buffer);
    } else {
        r->pattern = "";
    }

    nrule++;
    return 1;                   /* Rule found */
}

/* Read a rule file */
struct rule *parserulefile(FILE * f)
{
    char line[MAXLINE];
    struct rule *first_rule = NULL;
    struct rule **last_rule = &first_rule;
    struct rule *this_rule = tfmalloc(sizeof(struct rule));
    int rv;
    int lineno = 0;
    int err = 0;

    while (lineno++, fgets(line, MAXLINE, f)) {
        rv = parseline(line, this_rule, lineno);
        if (rv < 0)
            err = 1;
        if (rv > 0) {
            *last_rule = this_rule;
            last_rule = &this_rule->next;
            this_rule = tfmalloc(sizeof(struct rule));
        }
    }

    free(this_rule);            /* Last one is always unused */

    if (err) {
        /* Bail on error, we have already logged an error message */
        exit(EX_CONFIG);
    }

    return first_rule;
}

/* Destroy a rule file data structure */
void freerules(struct rule *r)
{
    struct rule *next;

    while (r) {
        next = r->next;

        regfree(&r->rx);

        /* "" patterns aren't allocated by malloc() */
        if (r->pattern && *r->pattern)
            free((void *)r->pattern);

        free(r);

        r = next;
    }
}

/* Execute a rule set on a string; returns a malloc'd new string. */
char *rewrite_string(const struct formats *pf,
                     const char *input, const struct rule *rules,
                     int mode, int af, match_pattern_callback macrosub,
                     const char **errmsg)
{
    char *current = tfstrdup(input);
    char *newstr, *newerstr;
    const char *accerr;
    const struct rule *ruleptr = rules;
    regmatch_t pmatch[10];
    int i;
    int len;
    int was_match = 0;
    int deadman = deadman_max_steps;
    int matchsense;
    int pmatches;
    unsigned int bad_flags;
    int ggoffset;

    /* Default error */
    *errmsg = "Remap table failure";

    if (verbosity >= 3) {
        syslog(LOG_INFO, "remap: input: %s", current);
    }

    bad_flags = 0;
    if (mode != RRQ)    bad_flags |= RULE_RRQ;
    if (mode != WRQ)    bad_flags |= RULE_WRQ;
    if (af != AF_INET)  bad_flags |= RULE_IPV4;
    if (af != AF_INET6) bad_flags |= RULE_IPV6;

    for (ruleptr = rules; ruleptr; ruleptr = ruleptr->next) {
        if (ruleptr->rule_flags & bad_flags)
            continue;		/* This rule is excluded by flags */

        matchsense = ruleptr->rule_flags & RULE_INVERSE ? REG_NOMATCH : 0;
        pmatches = ruleptr->rule_flags & RULE_INVERSE ? 0 : 10;

        /* Clear the pmatch[] array */
        for (i = 0; i < 10; i++)
            pmatch[i].rm_so = pmatch[i].rm_eo = -1;

        was_match = 0;

        do {
            if (!deadman--)
                goto dead;

            if (regexec(&ruleptr->rx, current, pmatches, pmatch, 0)
                != matchsense)
                break;          /* No match, break out of do loop */

            /* Match on this rule */
            was_match = 1;

            if (ruleptr->rule_flags & RULE_ABORT) {
                if (verbosity >= 3) {
                    syslog(LOG_INFO, "remap: rule %d: abort: %s",
                           ruleptr->nrule, current);
                }
                if (ruleptr->pattern[0]) {
                    /* Custom error message */
                    genmatchstring(&newstr, ruleptr->pattern, current,
                                   pmatch, macrosub, 0, NULL);
                    *errmsg = newstr;
                } else {
                    *errmsg = NULL;
                }
                free(current);
                return (NULL);
            }

            if (ruleptr->rule_flags & RULE_REWRITE) {
                len = genmatchstring(&newstr, ruleptr->pattern, current,
                                     pmatch, macrosub, 0, &ggoffset);

                if (ruleptr->rule_flags & RULE_SEDG) {
                    /* sed-style partial-matching global */
                    while (ggoffset < len &&
                           regexec(&ruleptr->rx, newstr + ggoffset,
                                   pmatches, pmatch,
                                   ggoffset ? REG_NOTBOL : 0)
                           == matchsense) {
                        if (!deadman--) {
                            free(current);
                            current = newstr;
                            goto dead;
                        }
                        len = genmatchstring(&newerstr, ruleptr->pattern,
                                             newstr, pmatch, macrosub,
                                             ggoffset, &ggoffset);
                        free(newstr);
                        newstr = newerstr;
                    }
                }

                if ((ruleptr->rule_flags & RULE_HASFILE) &&
                    pf->f_validate(newstr, mode, pf, &accerr)) {
                    if (verbosity >= 3) {
                        syslog(LOG_INFO, "remap: rule %d: ignored rewrite (%s): %s",
                               ruleptr->nrule, accerr, newstr);
                    }
                    free(newstr);
                    was_match = 0;
                    break;
                }

                free(current);
                current = newstr;
                if (verbosity >= 3) {
                    syslog(LOG_INFO, "remap: rule %d: rewrite: %s",
                           ruleptr->nrule, current);
                }
            } else if (ruleptr->rule_flags & RULE_HASFILE) {
                if (pf->f_validate(current, mode, pf, &accerr)) {
                    if (verbosity >= 3) {
                        syslog(LOG_INFO, "remap: rule %d: not exiting (%s)\n",
                               ruleptr->nrule, accerr);
                    }
                    was_match = 0;
                    break;
                }
            }
            /* If the rule is (old-style) global, keep going until no match */
        } while ((ruleptr->rule_flags & (RULE_GLOBAL|RULE_SEDG)) == RULE_GLOBAL);

        if (!was_match)
            continue;           /* Next rule */

        if (ruleptr->rule_flags & (RULE_EXIT|RULE_HASFILE)) {
            if (verbosity >= 3) {
                syslog(LOG_INFO, "remap: rule %d: exit",
                       ruleptr->nrule);
            }
            return current; /* Exit here, we're done */
        } else if (ruleptr->rule_flags & RULE_RESTART) {
            ruleptr = rules;        /* Start from the top */
            if (verbosity >= 3) {
                syslog(LOG_INFO, "remap: rule %d: restart",
                       ruleptr->nrule);
            }
        }
    }

    if (verbosity >= 3) {
        syslog(LOG_INFO, "remap: done");
    }
    return current;

dead:                           /* Deadman expired */
    syslog(LOG_ERR,
           "remap: Breaking loop after %d steps, input = %s, last = %s",
           deadman_max_steps, input, current);
    free(current);
    return NULL;        /* Did not terminate! */
}
