// The compiler driver: parses the command line, runs the pipeline for
// each C file, and drives the system assembler and linker.

#include "ouro.h"

#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

// From <mach-o/dyld.h>; declared here so that ouro's own headers do not
// need the Mach-O SDK headers.
int _NSGetExecutablePath(char *buf, uint32_t *bufsize);

bool opt_dump_ir;
bool opt_dump_ir_raw;
bool opt_dump_mir;
int opt_level = 1;
char *opt_base_file;

static bool opt_E;
static bool opt_S;
static bool opt_c;
static bool opt_v;
static bool opt_shared;
static char *opt_o;
static Vec input_files;
static Vec link_args;
static Vec tmp_files;
static Vec user_includes;
static Vec system_includes;
static Vec macro_ops; // "D<def>" or "U<name>", in command-line order

static void usage(int status) {
  fprintf(status ? stderr : stdout,
          "usage: ouro [options] file...\n"
          "  -o <file>      write output to <file>\n"
          "  -c             compile and assemble, do not link\n"
          "  -S             compile only; write assembly\n"
          "  -E             preprocess only\n"
          "  -O0, -O1       optimisation level (default -O1; -O2 and above mean -O1)\n"
          "  -I <dir>       add an include directory\n"
          "  -D <m>[=<v>]   define a macro\n"
          "  -U <m>         undefine a macro\n"
          "  -w             suppress warnings\n"
          "  -Werror        treat warnings as errors\n"
          "  --dump-ir      print the optimised IR of each function to stderr\n"
          "  --dump-ir-raw  print the IR before optimisation\n"
          "  --dump-mir     print machine code after register allocation\n"
          "  -shared        link a dynamic library\n"
          "  -l<lib> -L<dir> -Wl,<args>   passed to the linker\n"
          "  -v             show the commands that are run\n"
          "  --version      print the version\n");
  exit(status);
}

static char *take_arg(int argc, char **argv, int *i, const char *opt) {
  int n = strlen(opt);
  if (argv[*i][n])
    return argv[*i] + n;
  if (*i + 1 >= argc)
    fatal("missing argument to '%s'", opt);
  return argv[++*i];
}

static void parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    char *a = argv[i];
    if (a[0] != '-' || str_eq(a, "-")) {
      vec_push(&input_files, a);
      continue;
    }
    if (str_eq(a, "--help") || str_eq(a, "-h"))
      usage(0);
    if (str_eq(a, "--version")) {
      printf("ouro %s (arm64-apple-darwin)\n", OURO_VERSION);
      exit(0);
    }
    if (str_eq(a, "-o")) {
      opt_o = take_arg(argc, argv, &i, "-o");
      continue;
    }
    if (str_startswith(a, "-o")) {
      opt_o = a + 2;
      continue;
    }
    if (str_eq(a, "-E")) {
      opt_E = true;
      continue;
    }
    if (str_eq(a, "-P")) {
      opt_E_linemarkers = false;
      continue;
    }
    if (str_eq(a, "-S")) {
      opt_S = true;
      continue;
    }
    if (str_eq(a, "-c")) {
      opt_c = true;
      continue;
    }
    if (str_eq(a, "-shared") || str_eq(a, "-dynamiclib")) {
      opt_shared = true;
      continue;
    }
    if (str_eq(a, "-undefined")) {
      vec_push(&link_args, "-undefined");
      vec_push(&link_args, take_arg(argc, argv, &i, "-undefined"));
      continue;
    }
    if (str_eq(a, "-v")) {
      opt_v = true;
      continue;
    }
    if (str_eq(a, "-w")) {
      opt_warnings = false;
      continue;
    }
    if (str_eq(a, "-Werror")) {
      opt_werror = true;
      continue;
    }
    if (str_startswith(a, "-O")) {
      opt_level = (a[2] == '0') ? 0 : 1;
      continue;
    }
    if (str_startswith(a, "-I")) {
      vec_push(&user_includes, take_arg(argc, argv, &i, "-I"));
      continue;
    }
    if (str_eq(a, "-iquote")) {
      vec_push(&quote_dirs, take_arg(argc, argv, &i, "-iquote"));
      continue;
    }
    if (str_eq(a, "-isystem")) {
      vec_push(&system_includes, take_arg(argc, argv, &i, "-isystem"));
      continue;
    }
    if (str_startswith(a, "-D")) {
      vec_push(&macro_ops, fmt("D%s", take_arg(argc, argv, &i, "-D")));
      continue;
    }
    if (str_startswith(a, "-U")) {
      vec_push(&macro_ops, fmt("U%s", take_arg(argc, argv, &i, "-U")));
      continue;
    }
    if (str_eq(a, "--dump-ir")) {
      opt_dump_ir = true;
      continue;
    }
    if (str_eq(a, "--dump-ir-raw")) {
      opt_dump_ir_raw = true;
      continue;
    }
    if (str_eq(a, "--dump-mir")) {
      opt_dump_mir = true;
      continue;
    }
    if (str_startswith(a, "-l") || str_startswith(a, "-L")) {
      if (a[2]) {
        vec_push(&link_args, a);
      } else {
        char *v = take_arg(argc, argv, &i, a[1] == 'l' ? "-l" : "-L");
        vec_push(&link_args, fmt("%c%c%s", '-', a[1], v));
      }
      continue;
    }
    if (str_startswith(a, "-Wl,")) {
      char *s = xstrdup(a + 4);
      for (char *p = strtok(s, ","); p; p = strtok(NULL, ","))
        vec_push(&link_args, p);
      continue;
    }
    if (str_eq(a, "-framework")) {
      vec_push(&link_args, "-framework");
      vec_push(&link_args, take_arg(argc, argv, &i, "-framework"));
      continue;
    }
    // Options that do not apply to ouro and are accepted so that existing
    // build scripts work: warnings, language standard, debug info, code
    // generation flags.
    if (str_startswith(a, "-W") || str_startswith(a, "-std=") || str_startswith(a, "-g") ||
        str_startswith(a, "-f") || str_startswith(a, "-m") || str_eq(a, "-pedantic") ||
        str_eq(a, "-pedantic-errors") || str_eq(a, "-pipe") || str_eq(a, "-ansi") ||
        str_eq(a, "-pthread") || str_eq(a, "-rdynamic"))
      continue;
    if (str_eq(a, "-arch") || str_eq(a, "-x")) {
      take_arg(argc, argv, &i, a);
      continue;
    }
    fatal("unknown option: %s", a);
  }
  if (input_files.len == 0)
    fatal("no input files");
  if (opt_o && (opt_c || opt_S || opt_E) && input_files.len > 1)
    fatal("cannot specify -o with -c, -S or -E with multiple files");
}

//
// Temporary files and subprocesses
//

static void remove_tmp_files(void) {
  for (int i = 0; i < tmp_files.len; i++)
    unlink(tmp_files.data[i]);
}

static char *make_tmp(const char *suffix) {
  char *dir = getenv("TMPDIR");
  if (!dir || !*dir)
    dir = "/tmp";
  int n = strlen(dir);
  char *path = fmt("%s%souro-XXXXXX%s", dir, (n && dir[n - 1] == '/') ? "" : "/", suffix);
  int fd = mkstemps(path, strlen(suffix));
  if (fd < 0)
    fatal("cannot create a temporary file: %s", strerror(errno));
  close(fd);
  vec_push(&tmp_files, path);
  return path;
}

static void run(Vec *args) {
  vec_push(args, NULL);
  char **argv = (char **)args->data;
  if (opt_v) {
    for (int i = 0; argv[i]; i++)
      fprintf(stderr, "%s%s", i ? " " : "", argv[i]);
    fprintf(stderr, "\n");
  }
  pid_t pid = fork();
  if (pid < 0)
    fatal("fork failed: %s", strerror(errno));
  if (pid == 0) {
    execvp(argv[0], argv);
    fprintf(stderr, "ouro: cannot run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
  }
  int status;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      fatal("waitpid failed: %s", strerror(errno));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    fatal("%s failed", argv[0]);
}

static char *replace_ext(const char *path, const char *ext) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  const char *dot = strrchr(base, '.');
  int n = dot ? (int)(dot - base) : (int)strlen(base);
  return fmt("%.*s%s", n, base, ext);
}

// Finds the directory holding ouro's libc headers: the include/
// directory next to the executable or one or two levels up, so it works
// both from the build tree and from an installed prefix.
static char *find_system_include(void) {
  char *env = getenv("OURO_INCLUDE");
  if (env && *env)
    return env;
  char buf[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0)
    return NULL;
  char real[PATH_MAX];
  if (!realpath(buf, real))
    return NULL;
  char *dir = xstrdup(real);
  char *slash = strrchr(dir, '/');
  if (slash)
    *slash = '\0';
  const char *candidates[] = {"/include", "/../include", "/../../include", "/../share/ouro/include"};
  for (int i = 0; i < 4; i++) {
    char *p = fmt("%s%s", dir, candidates[i]);
    char *probe = fmt("%s/stdio.h", p);
    FILE *fp = fopen(probe, "r");
    if (fp) {
      fclose(fp);
      return p;
    }
  }
  return NULL;
}

static void setup_includes(void) {
  for (int i = 0; i < user_includes.len; i++) {
    IncludeDir *d = xalloc(sizeof(IncludeDir));
    d->dir = user_includes.data[i];
    vec_push(&include_dirs, d);
  }
  for (int i = 0; i < system_includes.len; i++) {
    IncludeDir *d = xalloc(sizeof(IncludeDir));
    d->dir = system_includes.data[i];
    d->is_system = true;
    vec_push(&include_dirs, d);
  }
  char *sys = find_system_include();
  if (!sys)
    fatal("cannot find ouro's include directory (set OURO_INCLUDE)");
  IncludeDir *d = xalloc(sizeof(IncludeDir));
  d->dir = sys;
  d->is_system = true;
  vec_push(&include_dirs, d);
}

//
// The pipeline
//

static void compile_to_asm(char *input, char *output) {
  opt_base_file = input;
  pp_init();
  for (int i = 0; i < macro_ops.len; i++) {
    char *op = macro_ops.data[i];
    if (op[0] == 'D')
      pp_define(op + 1);
    else
      pp_undef(op + 1);
  }
  Token *tok = preprocess_file(input);

  FILE *out;
  if (!output || str_eq(output, "-"))
    out = stdout;
  else if (!(out = fopen(output, "w")))
    fatal("cannot open output file '%s': %s", output, strerror(errno));

  if (opt_E) {
    print_preprocessed(out, tok);
    if (out != stdout)
      fclose(out);
    return;
  }

  convert_keywords_and_literals(tok);
  Program *prog = parse_program(tok);

  emit_begin(out, input);
  for (int i = 0; i < prog->funcs.len; i++) {
    Sym *fn = prog->funcs.data[i];
    Func *f = irgen_func(fn);
    if (opt_dump_ir_raw)
      ir_dump(stderr, f);
    optimize(f);
    if (opt_dump_ir)
      ir_dump(stderr, f);
    isel_func(f);
    regalloc_func(f);
    if (opt_dump_mir)
      mir_dump(stderr, f);
    emit_func(f);
  }
  emit_data(prog);
  emit_end();
  if (out != stdout && fclose(out) != 0)
    fatal("cannot write '%s': %s", output, strerror(errno));
}

static void assemble(char *input, char *output) {
  Vec args = {0};
  vec_push(&args, "as");
  vec_push(&args, "-arch");
  vec_push(&args, "arm64");
  vec_push(&args, "-o");
  vec_push(&args, output);
  vec_push(&args, input);
  run(&args);
}

static char *sdk_path(void) {
  char *env = getenv("SDKROOT");
  if (env && *env)
    return env;
  FILE *p = popen("xcrun --show-sdk-path 2>/dev/null", "r");
  if (!p)
    return NULL;
  char buf[PATH_MAX];
  char *res = NULL;
  if (fgets(buf, sizeof(buf), p)) {
    buf[strcspn(buf, "\n")] = '\0';
    res = xstrdup(buf);
  }
  pclose(p);
  return res;
}

static void link_objects(Vec *objs, char *output) {
  Vec args = {0};
  vec_push(&args, "ld");
  vec_push(&args, "-arch");
  vec_push(&args, "arm64");
  vec_push(&args, "-platform_version");
  vec_push(&args, "macos");
  vec_push(&args, "11.0.0");
  vec_push(&args, "11.0.0");
  char *sdk = sdk_path();
  if (sdk) {
    vec_push(&args, "-syslibroot");
    vec_push(&args, sdk);
  }
  if (opt_shared)
    vec_push(&args, "-dylib");
  vec_push(&args, "-o");
  vec_push(&args, output);
  for (int i = 0; i < objs->len; i++)
    vec_push(&args, objs->data[i]);
  for (int i = 0; i < link_args.len; i++)
    vec_push(&args, link_args.data[i]);
  vec_push(&args, "-lSystem");
  run(&args);
}

int main(int argc, char **argv) {
  atexit(remove_tmp_files);
  type_init();
  parse_args(argc, argv);
  setup_includes();

  Vec objs = {0};
  for (int i = 0; i < input_files.len; i++) {
    char *in = input_files.data[i];
    bool is_c = str_endswith(in, ".c") || str_eq(in, "-") || str_endswith(in, ".h");
    bool is_asm = str_endswith(in, ".s") || str_endswith(in, ".S");

    if (!is_c && !is_asm) {
      if (opt_E || opt_S || opt_c)
        continue;
      vec_push(&objs, in); // object files and libraries go to the linker
      continue;
    }

    if (opt_E) {
      compile_to_asm(in, opt_o);
      continue;
    }
    if (opt_S) {
      if (is_c)
        compile_to_asm(in, opt_o ? opt_o : replace_ext(in, ".s"));
      continue;
    }

    char *asm_file = in;
    if (is_c) {
      asm_file = make_tmp(".s");
      // Each C file is compiled in a fresh process image: the front end
      // keeps global state (macros, scopes), so compile one file per
      // child process.
      if (input_files.len > 1) {
        pid_t pid = fork();
        if (pid == 0) {
          // _exit, not exit: the parent's atexit handler would delete
          // the temporary files, including the one just written.
          compile_to_asm(in, asm_file);
          fflush(NULL);
          _exit(0);
        }
        int status;
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
          exit(1);
      } else {
        compile_to_asm(in, asm_file);
      }
    }
    if (opt_c) {
      assemble(asm_file, opt_o ? opt_o : replace_ext(in, ".o"));
      continue;
    }
    char *obj = make_tmp(".o");
    assemble(asm_file, obj);
    vec_push(&objs, obj);
  }

  if (!opt_E && !opt_S && !opt_c)
    link_objects(&objs, opt_o ? opt_o : "a.out");
  return 0;
}
