/* 
   Unix SMB/CIFS implementation.
   Samba utility functions
   Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __UTIL_ATTR_H__
#define __UTIL_ATTR_H__

/* for old gcc releases that don't have the feature test macro __has_attribute */
#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#ifndef _UNUSED_
#if __has_attribute(unused) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
/** gcc attribute used on function parameters so that it does not emit
 * warnings about them being unused. **/
#  define _UNUSED_ __attribute__ ((unused))
#else
#  define _UNUSED_
#endif
#endif
#ifndef UNUSED
#define UNUSED(param) param _UNUSED_
#endif

#ifndef _DEPRECATED_
#if __has_attribute(deprecated) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
#define _DEPRECATED_ __attribute__ ((deprecated))
#else
#define _DEPRECATED_
#endif
#endif

#ifndef _WARN_UNUSED_RESULT_
#if __has_attribute(warn_unused_result) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
#define _WARN_UNUSED_RESULT_ __attribute__ ((warn_unused_result))
#else
#define _WARN_UNUSED_RESULT_
#endif
#endif

#ifndef _NORETURN_
#if __has_attribute(noreturn) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
#define _NORETURN_ __attribute__ ((noreturn))
#else
#define _NORETURN_
#endif
#endif

#ifndef _PURE_
#if __has_attribute(pure) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
#define _PURE_ __attribute__((pure))
#else
#define _PURE_
#endif
#endif

#ifndef NONNULL
#if __has_attribute(nonnull) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
#define NONNULL(param) __attribute__((nonnull(param)))
#else
#define NONNULL(param)
#endif
#endif

#ifndef PRINTF_ATTRIBUTE
#if __has_attribute(format) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
/** Use gcc attribute to check printf fns.  a1 is the 1-based index of
 * the parameter containing the format, and a2 the index of the first
 * argument. Note that some gcc 2.x versions don't handle this
 * properly **/
#define PRINTF_ATTRIBUTE(a1, a2) __attribute__ ((format (__printf__, a1, a2)))
#else
#define PRINTF_ATTRIBUTE(a1, a2)
#endif
#endif

#ifndef FORMAT_ATTRIBUTE
#if __has_attribute(format) || ( (__GNUC__ >= 3) && (__GNUC_MINOR__ >= 1 ) )
/** Use gcc attribute to check printf fns.  a1 is argument to format()
 * in the above macro.  This is needed to support Heimdal's printf
 * decorations. Note that some gcc 2.x versions don't handle this
 * properly. **/
#define FORMAT_ATTRIBUTE(a) __attribute__ ((format a))
#else
#define FORMAT_ATTRIBUTE(a)
#endif
#endif

#endif /* __UTIL_ATTR_H__ */
