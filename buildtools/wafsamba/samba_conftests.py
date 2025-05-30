# a set of config tests that use the samba_autoconf functions
# to test for commonly needed configuration options

import os, shutil, re
from waflib import Build, Configure, Utils, Options, Logs, Errors
from waflib.Configure import conf
from samba_utils import TO_LIST, ADD_LD_LIBRARY_PATH, get_string

Options.OptionsContext.arg_list = []

def add_option(self, *k, **kw):
    '''syntax help: provide the "match" attribute to opt.add_option() so that folders can be added to specific config tests'''
    Options.OptionsContext.parser = self
    match = kw.get('match', [])
    if match:
        del kw['match']
    opt = self.parser.add_argument(*k, **kw)
    opt.match = match
    Options.OptionsContext.arg_list.append(opt)
    return opt
Options.OptionsContext.add_option = add_option

@conf
def check(self, *k, **kw):
    '''Override the waf defaults to inject --with-directory options'''

    if not 'env' in kw:
        kw['env'] = self.env.derive()

    # match the configuration test with specific options, for example:
    # --with-libiconv -> Options.options.iconv_open -> "Checking for library iconv"
    additional_dirs = []
    if 'msg' in kw:
        msg = kw['msg']
        for x in Options.OptionsContext.parser.parser.option_list:
             if getattr(x, 'match', None) and msg in x.match:
                 d = getattr(Options.options, x.dest, '')
                 if d:
                     additional_dirs.append(d)

    # we add the additional dirs twice: once for the test data, and again if the compilation test succeeds below
    def add_options_dir(dirs, env):
        for x in dirs:
             if not x in env.CPPPATH:
                 env.CPPPATH = [os.path.join(x, 'include')] + env.CPPPATH
             if not x in env.LIBPATH:
                 env.LIBPATH = [os.path.join(x, 'lib')] + env.LIBPATH

    add_options_dir(additional_dirs, kw['env'])

    self.validate_c(kw)
    self.start_msg(kw['msg'])
    ret = None
    try:
        ret = self.run_c_code(*k, **kw)
    except Configure.ConfigurationError as e:
        self.end_msg(kw['errmsg'], 'YELLOW')
        if 'mandatory' in kw and kw['mandatory']:
            if Logs.verbose > 1:
                raise
            else:
                self.fatal('the configuration failed (see %r)' % self.log.name)
    else:
        kw['success'] = ret
        self.end_msg(self.ret_msg(kw['okmsg'], kw))

        # success! keep the CPPPATH/LIBPATH
        add_options_dir(additional_dirs, self.env)

    self.post_check(*k, **kw)
    if not kw.get('execute', False):
        return ret == 0
    return ret


@conf
def CHECK_ICONV(conf, define='HAVE_NATIVE_ICONV'):
    '''check if the iconv library is installed
       optionally pass a define'''
    if conf.CHECK_FUNCS_IN('iconv_open', 'iconv', checklibc=True, headers='iconv.h'):
        conf.DEFINE(define, 1)
        return True
    return False


@conf
def CHECK_LARGEFILE(conf, define='HAVE_LARGEFILE'):
    '''see what we need for largefile support'''
    getconf_cflags = conf.CHECK_COMMAND(['getconf', 'LFS_CFLAGS'])
    if getconf_cflags is not False:
        if (conf.CHECK_CODE('if (sizeof(off_t) < 8) return 1',
                            define='WORKING_GETCONF_LFS_CFLAGS',
                            execute=True,
                            cflags=getconf_cflags,
                            msg='Checking getconf large file support flags work')):
            conf.ADD_CFLAGS(getconf_cflags)
            getconf_cflags_list=TO_LIST(getconf_cflags)
            for flag in getconf_cflags_list:
                if flag[:2] == "-D":
                    flag_split = flag[2:].split('=')
                    if len(flag_split) == 1:
                        conf.DEFINE(flag_split[0], '1')
                    else:
                        conf.DEFINE(flag_split[0], flag_split[1])

    if conf.CHECK_CODE('if (sizeof(off_t) < 8) return 1',
                       define,
                       execute=True,
                       msg='Checking for large file support without additional flags'):
        return True

    if conf.CHECK_CODE('if (sizeof(off_t) < 8) return 1',
                       define,
                       execute=True,
                       cflags='-D_FILE_OFFSET_BITS=64',
                       msg='Checking for -D_FILE_OFFSET_BITS=64'):
        conf.DEFINE('_FILE_OFFSET_BITS', 64)
        return True

    if conf.CHECK_CODE('if (sizeof(off_t) < 8) return 1',
                       define,
                       execute=True,
                       cflags='-D_LARGE_FILES',
                       msg='Checking for -D_LARGE_FILES'):
        conf.DEFINE('_LARGE_FILES', 1)
        return True
    return False


@conf
def CHECK_C_PROTOTYPE(conf, function, prototype, define, headers=None, msg=None, lib=None):
    '''verify that a C prototype matches the one on the current system'''
    if not conf.CHECK_DECLS(function, headers=headers):
        return False
    if not msg:
        msg = 'Checking C prototype for %s' % function
    return conf.CHECK_CODE('%s; void *_x = (void *)%s' % (prototype, function),
                           define=define,
                           local_include=False,
                           headers=headers,
                           link=False,
                           execute=False,
                           msg=msg,
                           lib=lib)


@conf
def CHECK_CHARSET_EXISTS(conf, charset, outcharset='UCS-2LE', headers=None, define=None):
    '''check that a named charset is able to be used with iconv_open() for conversion
    to a target charset
    '''
    msg = 'Checking if can we convert from %s to %s' % (charset, outcharset)
    if define is None:
        define = 'HAVE_CHARSET_%s' % charset.upper().replace('-','_')
    return conf.CHECK_CODE('''
                           iconv_t cd = iconv_open("%s", "%s");
                           if (cd == 0 || cd == (iconv_t)-1) return -1;
                           ''' % (charset, outcharset),
                           define=define,
                           execute=True,
                           msg=msg,
                           lib='iconv',
                           headers=headers)

def find_config_dir(conf):
    '''find a directory to run tests in'''
    k = 0
    while k < 10000:
        dir = os.path.join(conf.bldnode.abspath(), '.conf_check_%d' % k)
        try:
            shutil.rmtree(dir)
        except OSError:
            pass
        try:
            os.stat(dir)
        except:
            break
        k += 1

    try:
        os.makedirs(dir)
    except:
        conf.fatal('cannot create a configuration test folder %r' % dir)

    try:
        os.stat(dir)
    except:
        conf.fatal('cannot use the configuration test folder %r' % dir)
    return dir

@conf
def CHECK_SHLIB_INTRASINC_NAME_FLAGS(conf, msg):
    '''
        check if the waf default flags for setting the name of lib
        are ok
    '''

    snip = '''
int foo(int v) {
    return v * 2;
}
'''
    return conf.check(features='c cshlib',vnum="1",fragment=snip,msg=msg, mandatory=False)

@conf
def CHECK_NEED_LC(conf, msg):
    '''check if we need -lc'''

    dir = find_config_dir(conf)

    env = conf.env

    bdir = os.path.join(dir, 'testbuild2')
    if not os.path.exists(bdir):
        os.makedirs(bdir)


    subdir = os.path.join(dir, "liblctest")

    os.makedirs(subdir)

    Utils.writef(os.path.join(subdir, 'liblc1.c'), '#include <stdio.h>\nint lib_func(void) { FILE *f = fopen("foo", "r");}\n')

    bld = Build.BuildContext()
    bld.log = conf.log
    bld.all_envs.update(conf.all_envs)
    bld.all_envs['default'] = env
    bld.lst_variants = bld.all_envs.keys()
    bld.load_dirs(dir, bdir)

    bld.rescan(bld.srcnode)

    bld(features='c cshlib',
        source='liblctest/liblc1.c',
        ldflags=conf.env['EXTRA_LDFLAGS'],
        target='liblc',
        name='liblc')

    try:
        bld.compile()
        conf.check_message(msg, '', True)
        return True
    except:
        conf.check_message(msg, '', False)
        return False


@conf
def CHECK_SHLIB_W_PYTHON(conf, msg):
    '''check if we need -undefined dynamic_lookup'''

    dir = find_config_dir(conf)
    snip = '''
#include <Python.h>
#include <crt_externs.h>
#define environ (*_NSGetEnviron())

static PyObject *ldb_module = NULL;
int foo(int v) {
    extern char **environ;
    environ[0] = 1;
    ldb_module = PyImport_ImportModule("ldb");
    return v * 2;
}
'''
    return conf.check(features='c cshlib',uselib='PYEMBED',fragment=snip,msg=msg, mandatory=False)

# this one is quite complex, and should probably be broken up
# into several parts. I'd quite like to create a set of CHECK_COMPOUND()
# functions that make writing complex compound tests like this much easier
@conf
def CHECK_LIBRARY_SUPPORT(conf, rpath=False, version_script=False, msg=None):
    '''see if the platform supports building libraries'''

    if msg is None:
        if rpath:
            msg = "rpath library support"
        else:
            msg = "building library support"

    dir = find_config_dir(conf)

    bdir = os.path.join(dir, 'testbuild')
    if not os.path.exists(bdir):
        os.makedirs(bdir)

    env = conf.env

    subdir = os.path.join(dir, "libdir")

    os.makedirs(subdir)

    Utils.writef(os.path.join(subdir, 'lib1.c'), 'int lib_func(void) { return 42; }\n')
    Utils.writef(os.path.join(dir, 'main.c'),
                 'int lib_func(void);\n'
                 'int main(void) {return !(lib_func() == 42);}\n')

    bld = Build.BuildContext()
    bld.log = conf.log
    bld.all_envs.update(conf.all_envs)
    bld.all_envs['default'] = env
    bld.lst_variants = bld.all_envs.keys()
    bld.load_dirs(dir, bdir)

    bld.rescan(bld.srcnode)

    ldflags = []
    if version_script:
        ldflags.append("-Wl,--version-script=%s/vscript" % bld.path.abspath())
        Utils.writef(os.path.join(dir,'vscript'), 'TEST_1.0A2 { global: *; };\n')

    bld(features='c cshlib',
        source='libdir/lib1.c',
        target='libdir/lib1',
        ldflags=ldflags,
        name='lib1')

    o = bld(features='c cprogram',
            source='main.c',
            target='prog1',
            uselib_local='lib1')

    if rpath:
        o.rpath=os.path.join(bdir, 'default/libdir')

    # compile the program
    try:
        bld.compile()
    except:
        conf.check_message(msg, '', False)
        return False

    # path for execution
    lastprog = o.link_task.outputs[0].abspath(env)

    if not rpath:
        if 'LD_LIBRARY_PATH' in os.environ:
            old_ld_library_path = os.environ['LD_LIBRARY_PATH']
        else:
            old_ld_library_path = None
        ADD_LD_LIBRARY_PATH(os.path.join(bdir, 'default/libdir'))

    # we need to run the program, try to get its result
    args = conf.SAMBA_CROSS_ARGS(msg=msg)
    proc = Utils.subprocess.Popen([lastprog] + args,
            stdout=Utils.subprocess.PIPE, stderr=Utils.subprocess.PIPE)
    (out, err) = proc.communicate()
    w = conf.log.write
    w(str(out))
    w('\n')
    w(str(err))
    w('\nreturncode %r\n' % proc.returncode)
    ret = (proc.returncode == 0)

    if not rpath:
        os.environ['LD_LIBRARY_PATH'] = old_ld_library_path or ''

    conf.check_message(msg, '', ret)
    return ret



@conf
def CHECK_PERL_MANPAGE(conf, msg=None, section=None):
    '''work out what extension perl uses for manpages'''

    if msg is None:
        if section:
            msg = "perl man%s extension" % section
        else:
            msg = "perl manpage generation"

    conf.start_msg(msg)

    dir = find_config_dir(conf)

    bdir = os.path.join(dir, 'testbuild')
    if not os.path.exists(bdir):
        os.makedirs(bdir)

    Utils.writef(os.path.join(bdir, 'Makefile.PL'), """
use ExtUtils::MakeMaker;
WriteMakefile(
    'NAME'    => 'WafTest',
    'EXE_FILES' => [ 'WafTest' ]
);
""")
    back = os.path.abspath('.')
    os.chdir(bdir)
    proc = Utils.subprocess.Popen(['perl', 'Makefile.PL'],
                             stdout=Utils.subprocess.PIPE,
                             stderr=Utils.subprocess.PIPE)
    (out, err) = proc.communicate()
    os.chdir(back)

    ret = (proc.returncode == 0)
    if not ret:
        conf.end_msg('not found', color='YELLOW')
        return

    if section:
        man = Utils.readf(os.path.join(bdir,'Makefile'))
        m = re.search(r'MAN%sEXT\s+=\s+(\w+)' % section, man)
        if not m:
            conf.end_msg('not found', color='YELLOW')
            return
        ext = m.group(1)
        conf.end_msg(ext)
        return ext

    conf.end_msg('ok')
    return True


@conf
def CHECK_COMMAND(conf, cmd, msg=None, define=None, on_target=True, boolean=False):
    '''run a command and return result'''
    if msg is None:
        msg = 'Checking %s' % ' '.join(cmd)
    conf.COMPOUND_START(msg)
    cmd = cmd[:]
    if on_target:
        cmd.extend(conf.SAMBA_CROSS_ARGS(msg=msg))
    try:
        ret = get_string(Utils.cmd_output(cmd))
    except:
        conf.COMPOUND_END(False)
        return False
    if boolean:
        conf.COMPOUND_END('ok')
        if define:
            conf.DEFINE(define, '1')
    else:
        ret = ret.strip()
        conf.COMPOUND_END(ret)
        if define:
            conf.DEFINE(define, ret, quote=True)
    return ret


@conf
def CHECK_UNAME(conf):
    '''setup SYSTEM_UNAME_* defines'''
    ret = True
    for v in "sysname machine release version".split():
        if not conf.CHECK_CODE('''
                               int printf(const char *format, ...);
                               struct utsname n;
                               if (uname(&n) == -1) return -1;
                               printf("%%s", n.%s);
                               ''' % v,
                               define='SYSTEM_UNAME_%s' % v.upper(),
                               execute=True,
                               define_ret=True,
                               quote=True,
                               headers='sys/utsname.h',
                               local_include=False,
                               msg="Checking uname %s type" % v):
            ret = False
    return ret

@conf
def CHECK_INLINE(conf):
    '''check for the right value for inline'''
    conf.COMPOUND_START('Checking for inline')
    for i in ['inline', '__inline__', '__inline']:
        ret = conf.CHECK_CODE('''
        typedef int foo_t;
        static %s foo_t static_foo () {return 0; }
        %s foo_t foo () {return 0; }\n''' % (i, i),
                              define='INLINE_MACRO',
                              addmain=False,
                              link=False)
        if ret:
            if i != 'inline':
                conf.DEFINE('inline', i, quote=False)
            break
    if not ret:
        conf.COMPOUND_END(ret)
    else:
        conf.COMPOUND_END(i)
    return ret

@conf
def CHECK_XSLTPROC_MANPAGES(conf):
    '''check if xsltproc can run with the given stylesheets'''


    if not conf.CONFIG_SET('XSLTPROC'):
        conf.find_program('xsltproc', var='XSLTPROC')
    if not conf.CONFIG_SET('XSLTPROC'):
        return False

    s='http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl'
    conf.CHECK_COMMAND('%s --nonet %s 2> /dev/null' % (conf.env.get_flat('XSLTPROC'), s),
                             msg='Checking for stylesheet %s' % s,
                             define='XSLTPROC_MANPAGES', on_target=False,
                             boolean=True)
    if not conf.CONFIG_SET('XSLTPROC_MANPAGES'):
        print("A local copy of the docbook.xsl wasn't found on your system" \
              " consider installing package like docbook-xsl")

#
# Determine the standard libpath for the used compiler,
# so we can later use that to filter out these standard
# library paths when some tools like cups-config or
# python-config report standard lib paths with their
# ldflags (-L...)
#
@conf
def CHECK_STANDARD_LIBPATH(conf):
    # at least gcc and clang support this:
    try:
        cmd = conf.env.CC + ['-print-search-dirs']
        out = get_string(Utils.cmd_output(cmd)).split('\n')
    except ValueError:
        # option not supported by compiler - use a standard list of directories
        dirlist = [ '/usr/lib', '/usr/lib64' ]
    except:
        raise Errors.WafError('Unexpected error running "%s"' % (cmd))
    else:
        dirlist = []
        for line in out:
            line = line.strip()
            if line.startswith("libraries: ="):
                dirliststr = line[len("libraries: ="):]
                dirlist = [ os.path.normpath(x) for x in dirliststr.split(':') ]
                break

    conf.env.STANDARD_LIBPATH = dirlist

