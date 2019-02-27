/* File : fortran.cxx */
#include "swigmod.h"
#include "cparse.h"
#include <ctype.h>

#define ASSERT_OR_PRINT_NODE(COND, NODE) \
  do { \
    if (!(COND)) { \
      Printf(stdout, "********************************\n"); \
      Swig_print_node(NODE); \
      Printf(stdout, "Assertion '" #COND "' failed for node at %s:%d\n", Getfile(NODE), Getline(NODE)); \
      assert(COND); \
    } \
  } while (0)

namespace {
/* -------------------------------------------------------------------------
 * GLOBAL DATA
 * ------------------------------------------------------------------------- */

const char usage[] = "\
Fotran Options (available with -fortran)\n\
     -cppcast    - Enable C++ casting operators (default) \n\
     -nocppcast  - Disable C++ casting operators\n\
     -fext       - Change file extension of generated Fortran files to <ext>\n\
                   (default is f90)\n\
\n";

//! Maximum line length
const int g_max_line_length = 128;

const char g_fortran_end_statement[] = "\n";

/* -------------------------------------------------------------------------
 * UTILITY FUNCTIONS
 * ------------------------------------------------------------------------- */

/*!
 * \brief Whether a node is a constructor.
 *
 * Node should be a function
 */
bool is_node_constructor(Node *n) {
  return (Cmp(Getattr(n, "nodeType"), "constructor") == 0 || Getattr(n, "handled_as_constructor"));
}

/* -------------------------------------------------------------------------
 * \brief Print a comma-joined line of items to the given output.
 */
int print_wrapped_list(String *out, Iterator it, int line_length) {
  const char *prefix = "";
  for (; it.item; it = Next(it)) {
    line_length += 2 + Len(it.item);
    if (line_length >= g_max_line_length) {
      Printv(out, prefix, NULL);
      prefix = "&\n    ";
      line_length = 4 + Len(it.item);
    }
    Printv(out, prefix, it.item, NULL);
    prefix = ", ";
  }
  return line_length;
}

/* -------------------------------------------------------------------------
 * \brief Return a function wrapper for Fortran code.
 */
Wrapper *NewFortranWrapper() {
  Wrapper *w = NewWrapper();
  w->end_statement = g_fortran_end_statement;
  return w;
}

/* -------------------------------------------------------------------------
 * \brief Whether an expression is a standard base-10 integer compatible with
 * fortran
 *
 * Note that if it has a suffix e.g. `l` or `u`, or a prefix `0` (octal), it's
 * not compatible.
 *
 * Simple expressions like `1 + 2` are OK.
 */
bool is_fortran_intexpr(String *s) {
  const char *p = Char(s);
  char c = *p++;

  // Empty string is not an integer
  if (c == '\0')
    return false;

  // Allow leading negative sign
  if (c == '-')
    c = *p++;

  // Outer loop over words/tokens
  while (c) {
    // If it's a multi-digit number that starts with 0, it's octal, and thus
    // not a simple integer
    if (c == '0' && *p != 0)
      return false;

    while (c) {
      if (!isdigit(c))
        return false;

      c = *p++;
    }
  }
  return true;
}

/* -------------------------------------------------------------------------
 * \brief Check a parameter for invalid dimension names.
 */
bool bad_fortran_dims(Node *n, const char *tmap_name) {
  bool is_bad = false;
  // See if the typemap needs its dimensions checked
  String *key = NewStringf("tmap:%s:checkdim", tmap_name);
  if (GetFlag(n, key)) {
    SwigType *t = Getattr(n, "type");
    if (SwigType_isarray(t)) {
      int ndim = SwigType_array_ndim(t);
      for (int i = 0; i < ndim; i++) {
        String *dim = SwigType_array_getdim(t, i);
        if (dim && Len(dim) > 0 && !is_fortran_intexpr(dim)) {
          Swig_warning(WARN_LANG_IDENTIFIER, input_file, line_number,
                       "Array dimension expression '%s' is incompatible with Fortran\n",
                       dim);
          is_bad = true;
        }
        Delete(dim);
      }
    }
  }

  Delete(key);
  return is_bad;
}

/* -------------------------------------------------------------------------
 * \brief Determine whether to wrap an enum as a value.
 */
bool is_native_enum(Node *n) {
  String *enum_feature = Getattr(n, "feature:fortran:const");
  if (!enum_feature) {
    // Determine from enum values
    for (Node *c = firstChild(n); c; c = nextSibling(c)) {
      if (Getattr(c, "error") || GetFlag(c, "feature:ignore")) {
        return false;
      }

      String *enum_value = Getattr(c, "enumvalue");
      if (enum_value && !is_fortran_intexpr(enum_value)) {
        return false;
      }
    }
    // No bad values
    return true;
  } else if (Strcmp(enum_feature, "0") == 0) {
    // User forced it not to be a native enum
    return false;
  } else {
    // %fortranconst was set as a flag
    return true;
  }
}

/* -------------------------------------------------------------------------
 * \brief Determine whether to wrap an enum as a value.
 */
bool is_native_parameter(Node *n) {
  String *param_feature = Getattr(n, "feature:fortran:const");
  if (!param_feature) {
    // Default to not wrapping natively
    return false;
  } else if (Strcmp(param_feature, "0") == 0) {
    // Not a native param
    return false;
  } else {
    // Value specified and isn't "0"
    return true;
  }
}

/* -------------------------------------------------------------------------
 * Construct a specifier suffix from a BIND(C) typemap.
 * 
 * This returns NULL if the typestr doesn't have a simple KIND, otherwise
 * returns a newly allocated String with the suffix.
 *
 * TODO: consider making this a typedef
 */
String *make_specifier_suffix(String *bindc_typestr) {
  String *suffix = NULL;
    // Search for the KIND embedded in `real(C_DOUBLE)` so that we can
    // append the fortran specifier. This is kind of a hack, but native
    // parameters should really only be used for the kinds we define in
    // fortypemaps.swg
    const char *start = Char(bindc_typestr);
    const char *stop = start + Len(bindc_typestr);
    // Search forward for left parens
    for (; start != stop; ++start) {
      if (*start == '(') {
        ++start;
        break;
      }
    }
    // Search backward for right parens
    for (; stop != start; --stop) {
      if (*stop == ')') {
        break;
      }
    }

    if (stop != start) {
      suffix = NewStringWithSize(start, (int)(stop - start));
    }
    return suffix;
}

/* -------------------------------------------------------------------------
 * \brief Determine whether to wrap a function/class as a c-bound struct
 * or function.
 */
bool is_bindc(Node *n) {
  bool result = GetFlag(n, "feature:fortran:bindc");
  if (result && CPlusPlus) {
    String *kind = Getattr(n, "kind");
    if (kind && Strcmp(kind, "function") == 0 && !Swig_storage_isexternc(n)) {
      Swig_error(input_file,
                 line_number,
                 "The C++ function '%s' is not defined with external "
                 "C linkage (extern \"C\"), but it is marked with %%fortran_bindc.\n",
                 Getattr(n, "sym:name"));
    }
  }
  return result;
}

/* -------------------------------------------------------------------------
 * \brief Whether an SWIG type can be rendered as TYPE VAR.
 *
 * Some declarations (arrays, function pointers, member function pointers)
 * require the variable to be embedded in the middle of the array and thus
 * require special treatment.
 */
bool return_type_needs_typedef(String *s) {
  String *strprefix = SwigType_prefix(s);
  bool result = (Strstr(strprefix, "p.a(") || Strstr(strprefix, "p.f(") || Strstr(strprefix, "p.m("));
  Delete(strprefix);
  return result;
}

/* -------------------------------------------------------------------------
 * Get or create a list 
 *
 * This only applies while a class is being wrapped to methods in that particular class.
 */
List *get_default_list(Node *n, String *key) {
  assert(n);
  List *result = Getattr(n, key);
  if (!result) {
    result = NewList();
    Setattr(n, key, result);
  }
  return result;
}

/* -------------------------------------------------------------------------
 * \brief Get some name attached to the node.
 *
 * This is for user feedback only.
 */
String *get_symname_or_name(Node *n) {
  String *s = Getattr(n, "sym:name");
  if (!s) {
    s = Getattr(n, "name");
  }
  return s;
}

/* -------------------------------------------------------------------------
 * \brief Construct any necessary 'import' identifier.
 *
 * When the `imtype` is an actual `type(Foo)`, it's necessary to import the identifier Foo from the module definition scope. This function examines the
 * evaluated `imtype` (could be `imtype:in`, probably has $fclassname replaced)
 */
String *make_import_string(String *imtype) {
  char *start = Strstr(imtype, "type(");
  if (start == NULL)
    return NULL; // No 'type('

  start += 5; // Advance to whatever comes after 'type'
  char *end = start;
  while (*end != '\0' && *end != ')')
    ++end;

  // Create a substring and convert to lowercase
  String* result = NewStringWithSize(start, end - start);
  for (char* c = Char(result); *c != '\0'; ++c)
    *c = tolower(*c);

  if (Strcmp(result, "c_ptr") == 0
      || Strcmp(result, "c_funptr") == 0)
  {
    // Don't import types pulled in from `use, intrinsic :: ISO_C_BINDING`
    Delete(result);
    result = NULL;
  }

  return result;
}

/* -------------------------------------------------------------------------
 * \brief Whether a name is a valid fortran identifier
 */
bool is_valid_identifier(const_String_or_char_ptr name) {
  const char *c = Char(name);
  if (*c == '\0')
    return false;
  if (*c == '_')
    return false;
  if (*c >= '0' && *c <= '9')
    return false;
  if (Len(name) > 63)
    return false;
  return true;
}

/* -------------------------------------------------------------------------
 * \brief Make a string shorter by hashing its end.
 *
 * Requires input to be longer than 63 chars.
 * Returns new'd string.
 */
String *shorten_identifier(String *inp, int warning = WARN_NONE) {
  assert(Len(inp) > 63);
  String *result = NewStringWithSize(inp, 63);
  unsigned int hash = 5381;
  // Hash truncated characters *AND* characters that might be replaced by the hash
  // (2**8 / (10 + 26)) =~ 7.1, so backtrack 8 chars
  for (const char *src = Char(inp) + 63 - 8; *src != '\0'; ++src) {
    hash = (hash * 33 + *src) & 0xffffffffu;
  }
  // Replace the last chars with the hash encoded into 0-10 + A-Z
  char *dst = Char(result) + 63;
  while (hash > 0) {
    unsigned long rem = hash % 36;
    hash = hash / 36;
    *dst-- = (rem < 10 ? '0' + rem : ('A' + rem - 10));
  }

  if (warning != WARN_NONE && !Getmeta(inp, "already_warned")) {
    Swig_warning(warning, input_file, line_number, "Fortran identifiers may be no longer than 64 characters: renaming '%s' to '%s'\n", inp, result);
    Setmeta(inp, "already_warned", "1");
  }
  return result;
}

/* -------------------------------------------------------------------------
 * \brief If a string is too long, shorten it. Otherwise leave it.
 *
 * This should only be used for strings whose beginnings are valid fortran
 * identifiers -- e.g. strings that we construct.
 *
 * *assumes ownership of input and returns new'd value*
 */
String *ensure_short(String *str, int warning = WARN_NONE) {
  if (Len(str) > 63) {
    String *shortened = shorten_identifier(str, warning);
    assert(is_valid_identifier(shortened));
    Delete(str);
    str = shortened;
  }
  return str;
}

/* -------------------------------------------------------------------------
 * \brief If a string is too long, shorten it. Otherwise leave it.
 *
 * This should only be used for strings whose beginnings are valid fortran
 * identifiers -- e.g. strings that we construct.
 *
 * *assumes ownership of input and returns new'd value*
 */
String *proxy_name_construct(String *nspace, const_String_or_char_ptr classname, const_String_or_char_ptr symname) {
  String *result;
  if (nspace && classname) {
    result = NewStringf("swigf_%s_%s_%s", nspace, classname, symname);
  } else if (nspace || classname) {
    result = NewStringf("swigf_%s_%s", nspace ? nspace : classname, symname);
  } else {
    result = NewStringf("swigf_%s", symname);
  }
  return ensure_short(result);
}

String *proxy_name_construct(String *nspace, const_String_or_char_ptr symname) {
  return proxy_name_construct(nspace, NULL, symname);
}

/* -------------------------------------------------------------------------
 * \brief Change a symname to a valid Fortran identifier, warn if changing
 *
 * The maximum length of a Fortran identifier is 63 characters, according
 * to the Fortran standard.
 *
 * \return new'd valid identifier name
 */
String *make_fname(String *name, int warning = WARN_LANG_IDENTIFIER) {
  assert(name);
  String* result = NULL;

  // Move underscores and leading digits to the end of the string
  const char *start = Char(name);
  const char *c = start;
  const char *stop = start + Len(name);
  while (c != stop && (*c == '_' || (*c >= '0' && *c <= '9'))) {
    ++c;
  }
  if (c != start) {
    // Move invalid characters to the back of the string
    if (c == stop) {
      // No valid characters, e.g. _1234; prepend an 'f'
      result = NewString("f");
    } else {
      result = NewStringWithSize(c, (int)(stop - c));
    }
    String *tail = NewStringWithSize(start, (int)(c - start));
    Printv(result, tail, NULL);
    Delete(tail);

    if (warning != WARN_NONE && !Getmeta(name, "already_warned")) {
      Swig_warning(warning, input_file, line_number,
                   "Fortran identifiers may not begin with underscores or numerals: renaming '%s' to '%s'\n",
                   name, result);
    }
    Setmeta(name, "already_warned", "1");
  }

  // The beginning of the string is set up; now capture and shorten if too long
  result = ensure_short(result ? result : Copy(name), warning);
  
  assert(is_valid_identifier(result));
  return result;
}
/* -------------------------------------------------------------------------
 * \brief Get/attach and return a typemap to the given node.
 *
 * If 'ext' is non-null, then after binding/searchinbg, a search will be made
 * for the typemap with the given extension. If that's present, it's used
 * instead of the default typemap. (This allows overriding of e.g. 'tmap:ctype'
 * with 'tmap:ctype:in'.)
 *
 * If 'warning' is WARN_NONE, then if the typemap is not found, the return
 * value will be NULL. Otherwise a mangled typename will be created and saved
 * to attributes (or if attributes is null, then the given node).
 */
String *get_typemap(const_String_or_char_ptr tmname, const_String_or_char_ptr ext, Node *n, int warning, bool attach) {
  assert(tmname);
  String *result = NULL;
  String *key = NewStringf("tmap:%s", tmname);

  if (attach) {
    // Attach the typemap, or NULL if it's not there
    String *lname = Getattr(n, "lname");
    if (!lname)
      lname = Getattr(n, "name");
    assert(lname);
    result = Swig_typemap_lookup(tmname, n, lname, NULL);
  } else {
    // Look up a typemap that should already be attached
    result = Getattr(n, key);
  }

  if (!result && warning != WARN_NONE) {
    // Typemap was not found: emit a warning
    SwigType *type = Getattr(n, "type");
    if (!type) {
      type = Getattr(n, "name");
    }
    if (!type) {
      type = NewString("UNKNOWN");
    }
    Swig_warning(warning, Getfile(n), Getline(n),
                 "No '%s' typemap defined for %s\n",
                 tmname, SwigType_str(type, 0));

    String *tmap_match_key = NewStringf("tmap:%s:match_type", tmname);
    Setattr(n, tmap_match_key, "SWIGTYPE");
    Delete(tmap_match_key);
  }

  if (ext) {
    String *tempkey = NewStringf("tmap:%s:%s", tmname, ext);
    String *suffixed_tm = Getattr(n, tempkey);
    if (suffixed_tm) {
      // Replace the output value with the specialization
      result = suffixed_tm;
      // Replace the key with the specialized key
      Delete(key);
      key = tempkey;
      tempkey = NULL;
    }
    Delete(tempkey);
  }

  Delete(key);
  return result;
}

/* ------------------------------------------------------------------------- */ 
//! Attach and return a typemap to the given node.
String *attach_typemap(const_String_or_char_ptr tmname, Node *n, int warning) {
  return get_typemap(tmname, NULL, n, warning, true);
}

//! Get and return a typemap to the given node.
String *get_typemap(const_String_or_char_ptr tmname, Node *n, int warning) {
  return get_typemap(tmname, NULL, n, warning, false);
}

//! Get and return a typemap (with extension) to the given node.
String *get_typemap(const_String_or_char_ptr tmname, const_String_or_char_ptr ext, Node *n, int warning) {
  return get_typemap(tmname, ext, n, warning, false);
}

/* -------------------------------------------------------------------------
 * \brief Get a plain-text type like "int *", convert it to "p.int"
 *
 * This also sets the attribute in the node.
 *
 * This function is exclusively used for the "tmap:ctype" attribute, which
 * the user inputs as a plain-text C declaration but doesn't automatically get
 * converted by the SWIG type system like the "type" attribute does.
 *
 * Caller is responsible for calling Delete on the return value. Will return
 * NULL if the typemap isn't defined.
 */
SwigType *parse_typemap(const_String_or_char_ptr tmname, const_String_or_char_ptr ext, Node *n, int warning) {
  // Get the typemap, which has the *unparsed and unsimplified* type
  String *raw_tm = get_typemap(tmname, ext, n, warning);
  // Convert the plain-text string to a SWIG type
  SwigType *parsed_type = Swig_cparse_type(raw_tm);
  if (!parsed_type) {
    return NULL;
  }

  // Replace the contents of the original typemap string with the parsed
  // result -- this is a sort of hack for avoiding the 'Setattr(tmname,
  // resolved_type)' where we'd have to recalculate the tmname key again
  Clear(raw_tm);
  Printv(raw_tm, parsed_type, NULL);
  Delete(parsed_type);
  return raw_tm;
}

SwigType *parse_typemap(const_String_or_char_ptr tmname, Node *n, int warning) {
  return parse_typemap(tmname, NULL, n, warning);
}

//---------------------------------------------------------------------------//
// Swig_fragment_emit can't be called with a const char* argument.
void emit_fragment(const char *name) {
  String *temp = NewString(name);
  Swig_fragment_emit(temp);
  Delete(temp);
}

/* ------------------------------------------------------------------------- */
} // end anonymous namespace

class FORTRAN : public Language {
private:
  // >>> OUTPUT FILES

  // Injected into .cxx file
  String *f_begin;   //!< Very beginning of output file
  String *f_runtime; //!< SWIG runtime code
  String *f_policies;//!< AssignmentType flags for each class 
  String *f_header;  //!< Declarations and inclusions from .i
  String *f_wrapper; //!< C++ Wrapper code
  String *f_init;    //!< C++ initalization functions

  // Injected into module file
  String *f_fbegin;      //!< Very beginning of output file
  String *f_fuse;        //!< Fortran "use" directives
  String *f_fdecl;       //!< Module declaration constructs
  String *f_finterfaces; //!< Fortran interface declarations to SWIG functions
  String *f_fsubprograms;    //!< Fortran subroutine wrapper functions

  // Keep track of anonymous classes and enums
  Hash *d_mangled_type;

  // Module-wide procedure interfaces
  Hash *d_overloads; //!< Overloaded subroutine -> overload names

  // Current class parameters
  String *f_class;          //!< Proxy code in currently generated class
  Hash *d_method_overloads; //!< Overloaded subroutine -> overload names
  List *d_constructors;     //!< Overloaded subroutine -> overload names

  // Inside of the 'enum' definitions
  List *d_enum_public; //!< List of enumerator values

  // >>> CONFIGURE OPTIONS

  String *d_fext; //!< Fortran file extension

public:
  virtual void main(int argc, char *argv[]);
  virtual int top(Node *n);
  virtual int moduleDirective(Node *n);
  virtual int functionWrapper(Node *n);
  virtual int destructorHandler(Node *n);
  virtual int constructorHandler(Node *n);
  virtual int classDeclaration(Node *n);
  virtual int classHandler(Node *n);
  virtual int memberfunctionHandler(Node *n);
  virtual int membervariableHandler(Node *n);
  virtual int globalvariableHandler(Node *n);
  virtual int staticmemberfunctionHandler(Node *n);
  virtual int staticmembervariableHandler(Node *n);
  virtual int enumDeclaration(Node *n);
  virtual int constantWrapper(Node *n);
  virtual int classforwardDeclaration(Node *n);
  virtual int enumforwardDeclaration(Node *n);

  virtual String *makeParameterName(Node *n, Parm *p, int arg_num, bool is_setter = false) const;
  virtual void replaceSpecialVariables(String *method, String *tm, Parm *parm);

  FORTRAN() : d_mangled_type(NULL), d_overloads(NULL), f_class(NULL), d_method_overloads(NULL), d_constructors(NULL), d_enum_public(NULL) {}

private:
  int cfuncWrapper(Node *n);
  int bindcfuncWrapper(Node *n);
  int imfuncWrapper(Node *n);
  int proxyfuncWrapper(Node *n);
  void write_docstring(Node *n, String *dest);

  void write_wrapper(String *filename);
  void write_module(String *filename);

  bool replace_fclassname(SwigType *type, String *tm);
  String *get_fclassname(SwigType *classnametype, bool is_enum);

  // Add an assignment operator to a class node
  void add_assignment_operator(Node *n);

  // Add lowercase symbol (fortran)
  int add_fsymbol(String *s, Node *n, int warning = WARN_FORTRAN_NAME_CONFLICT);
  // Create a unique symbolic name
  String *make_unique_symname(Node *n);
  // Whether the current class is a BIND(C) struct
  bool is_bindc_struct() const { assert(this->getCurrentClass()); return d_method_overloads == NULL; }
};

/* -------------------------------------------------------------------------
 * \brief Main function for code generation.
 */
void FORTRAN::main(int argc, char *argv[]) {
  int cppcast = 1;

  /* Set language-specific subdirectory in SWIG library */
  SWIG_library_directory("fortran");

  // Default string extension
  d_fext = NewString("f90");

  // Set command-line options
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-cppcast") == 0) {
      cppcast = 1;
      Swig_mark_arg(i);
    } else if (strcmp(argv[i], "-nocppcast") == 0) {
      cppcast = 0;
      Swig_mark_arg(i);
    } else if (strcmp(argv[i], "-fext") == 0) {
      Swig_mark_arg(i);
      if (argv[i + 1]) {
        Delete(d_fext);
        d_fext = NewString(argv[i + 1]);
        Swig_mark_arg(i + 1);
        ++i;
      } else {
        Swig_arg_error();
      }
    } else if ((strcmp(argv[i], "-help") == 0)) {
      Printv(stdout, usage, NULL);
    }
  }

  /* Enable C++ casting */
  if (cppcast) {
    Preprocessor_define("SWIG_CPLUSPLUS_CAST", 0);
  }

  /* Set language-specific preprocessing symbol */
  Preprocessor_define("SWIGFORTRAN 1", 0);

  /* Set typemap language (historical) */
  SWIG_typemap_lang("fortran");

  /* Set language-specific configuration file */
  SWIG_config_file("fortran.swg");

  allow_overloading();
  Swig_interface_feature_enable();
}

/* -------------------------------------------------------------------------
 * \brief Top-level code generation function.
 */
int FORTRAN::top(Node *n) {
  // Configure output filename using the name of the SWIG input file
  String *foutfilename = NewStringf("%s%s.%s", SWIG_output_directory(), Getattr(n, "name"), d_fext);
  Setattr(n, "fortran:outfile", foutfilename);
  Delete(foutfilename);

  // >>> C++ WRAPPER CODE

  // run time code (beginning of .cxx file)
  f_begin = NewStringEmpty();
  Swig_register_filebyname("begin", f_begin);

  // run time code (beginning of .cxx file)
  f_runtime = NewStringEmpty();
  Swig_register_filebyname("runtime", f_runtime);

  f_policies = NewStringEmpty();

  // header code (after run time)
  f_header = NewStringEmpty();
  Swig_register_filebyname("header", f_header);

  // C++ wrapper code (middle of .cxx file)
  f_wrapper = NewStringEmpty();
  Swig_register_filebyname("wrapper", f_wrapper);

  // initialization code (end of .cxx file)
  f_init = NewStringEmpty();
  Swig_register_filebyname("init", f_init);

  // >>> FORTRAN WRAPPER CODE

  // Code before the `module` statement
  f_fbegin = NewStringEmpty();
  Swig_register_filebyname("fbegin", f_fbegin);

  // Start of module:
  f_fuse = NewStringEmpty();
  Swig_register_filebyname("fuse", f_fuse);

  // Module declarations
  f_fdecl = NewStringEmpty();
  Swig_register_filebyname("fdecl", f_fdecl);

  // Fortran BIND(C) interfavces
  f_finterfaces = NewStringEmpty();
  Swig_register_filebyname("finterfaces", f_finterfaces);

  // Fortran subroutines (proxy code)
  f_fsubprograms = NewStringEmpty();
  Swig_register_filebyname("fsubprograms", f_fsubprograms);

  d_mangled_type = NewHash();
  d_overloads = NewHash();

  // Declare scopes: fortran types and forward-declared types
  this->symbolAddScope("fortran");

  // Emit all other wrapper code
  Language::top(n);

  // Write C++ wrapper file
  write_wrapper(Getattr(n, "outfile"));

  // Write fortran module file
  write_module(Getattr(n, "fortran:outfile"));

  // Clean up files and other data
  Delete(d_overloads);
  Delete(d_mangled_type);
  Delete(f_fsubprograms);
  Delete(f_finterfaces);
  Delete(f_fdecl);
  Delete(f_fuse);
  Delete(f_init);
  Delete(f_wrapper);
  Delete(f_header);
  Delete(f_policies);
  Delete(f_runtime);
  Delete(f_fbegin);
  Delete(f_begin);

  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Write C++ wrapper code
 */
void FORTRAN::write_wrapper(String *filename) {
  // Open file
  File *out = NewFile(filename, "w", SWIG_output_files());
  if (!out) {
    FileErrorDisplay(filename);
    SWIG_exit(EXIT_FAILURE);
  }

  // Write SWIG auto-generation banner
  Swig_banner(out);

  // Write three different levels of output
  Dump(f_begin, out);
  Dump(f_runtime, out);
  Dump(f_policies, out);
  Dump(f_header, out);

  // Write wrapper code
  if (CPlusPlus)
    Printf(out, "extern \"C\" {\n");
  Dump(f_wrapper, out);
  if (CPlusPlus)
    Printf(out, "} // extern\n");

  // Write initialization code
  Wrapper_pretty_print(f_init, out);

  // Close file
  Delete(out);
}

/* -------------------------------------------------------------------------
 * \brief Write Fortran implementation module
 */
void FORTRAN::write_module(String *filename) {
  // Open file
  File *out = NewFile(filename, "w", SWIG_output_files());
  if (!out) {
    FileErrorDisplay(filename);
    SWIG_exit(EXIT_FAILURE);
  }

  // Write SWIG auto-generation banner
  Swig_banner_target_lang(out, "!");

  // Write module
  Dump(f_fbegin, out);
  Dump(f_fuse, out);
  Printv(out,
         " implicit none\n"
         " private\n",
         NULL);

  // Types and such
  Printv(out, "\n ! DECLARATION CONSTRUCTS\n", f_fdecl, NULL);

  // Overloads and renamed module procedures
  for (Iterator kv = First(d_overloads); kv.key; kv = Next(kv)) {
    Printv(out,
           " interface ", kv.key, "\n"
           "  module procedure ",
           NULL);

    // Write overloaded procedure names
    int line_length = 19;
    line_length = print_wrapped_list(out, First(kv.item), line_length);
    Printv(out,
           "\n"
           " end interface\n"
           " public :: ", kv.key, "\n",
           NULL);
  }

  if (Len(f_finterfaces) > 0) {
    Printv(out,
           "\n! WRAPPER DECLARATIONS\n"
           "interface\n",
           f_finterfaces,
           "end interface\n"
           "\n",
           NULL);
  }
  if (Len(f_fsubprograms) > 0) {
    Printv(out,
           "\ncontains\n"
           " ! MODULE SUBPROGRAMS\n",
           f_fsubprograms,
           NULL);
  }
  Printv(out, "\nend module", "\n", NULL);

  // Close file
  Delete(out);
}

/* -------------------------------------------------------------------------
 * \brief Process a %module
 */
int FORTRAN::moduleDirective(Node *n) {
  String *modname = Swig_string_lower(Getattr(n, "name"));
  int success = this->add_fsymbol(modname, n, WARN_NONE);

  if (ImportMode) {
    // This %module directive is inside another module being %imported
    Printv(f_fuse, " use ", modname, "\n", NULL);
    success = SWIG_OK;
  } else if (success) {
    // This is the first time the `%module` directive is seen. (Note that
    // other `%module` directives may be present, but they're
    // given the same name as the main module and should be ignored.
    // Write documentation if given. Note that it's simply labeled "docstring"
    // and in a daughter node; to unify the doc string processing we just set
    // it as a feature attribute on the module.
    Node *options = Getattr(n, "options");
    if (options) {
      String *docstring = Getattr(options, "docstring");
      if (docstring) {
        Setattr(n, "feature:docstring", docstring);
        this->write_docstring(n, f_fuse);
      }
    }

    Printv(f_fuse,
           "module ",
           modname,
           "\n"
           " use, intrinsic :: ISO_C_BINDING\n",
           NULL);
  }

  Delete(modname);
  return success;
}

/* -------------------------------------------------------------------------
 * \brief Wrap basic functions.
 *
 * This is called from many different handlers, including:
 *  - member functions
 *  - member variables (once each for get&set)
 *  - global variables (once each for get&set)
 *  - static functions
 */
int FORTRAN::functionWrapper(Node *n) {
  const bool bindc = is_bindc(n);
  const bool member = GetFlag(n, "fortran:ismember");
  bool generic = false;

  // >>> SET UP WRAPPER NAME

  String *symname = Getattr(n, "sym:name");
  String *fsymname = NULL; // Fortran name alias (or member function name)
  String *fname = NULL;    // Fortran proxy function name; null if bind(C)
  String *imname = NULL;   // Fortran interface function name
  String *wname = NULL;    // SWIG C wrapper function name

  if (!bindc) {
    // Usual case: generate a unique wrapper name
    wname = Swig_name_wrapper(symname);
    imname = ensure_short(NewStringf("swigc_%s", symname));

    if (String *private_fname = Getattr(n, "fortran:fname")) {
      // Create "private" fortran wrapper function class (swigf_xx) name that will be bound to a class
      fname = Copy(private_fname);
      ASSERT_OR_PRINT_NODE(is_valid_identifier(fname), n);
    } else if (String *varname = Getattr(n, "fortran:variable")) {
      const char* prefix = (GetFlag(n, "memberset") || GetFlag(n, "varset")) ? "set" : "get";
      fname = ensure_short(NewStringf("%s_%s", prefix, varname));
      
      if (member) {
        // We're wrapping a static/member variable. The getter/setter name is an alias to the class-namespaced proxy function.
        fsymname = fname;
        fname = proxy_name_construct(this->getNSpace(), Getattr(n, "sym:name"));
      }
    } else {
      // Default: use symbolic function name
      fname = make_fname(symname, WARN_NONE);
    }
  } else {
    // BIND(C): use *original* C function name to generate the interface to, and create an acceptable
    // Fortran identifier based on whatever renames have been requested.
    wname = Copy(Getattr(n, "name"));
    imname = make_fname(symname, WARN_NONE);
    fname = NULL;
  }

  if (String *manual_name = Getattr(n, "feature:fortran:generic")) {
    // Override the fsymname name for this function
    assert(!fsymname);
    fsymname = Copy(manual_name);
    generic = true;
  } else if (String *manual_name = Getattr(n, "fortran:name")) {
    // Override the fsymname name for this function
    assert(!fsymname);
    fsymname = Copy(manual_name);
  }

  // Add suffix if the function is overloaded (can't overload C bound functions)
  if (String *overload_ext = (Getattr(n, "sym:overloaded") ? Getattr(n, "sym:overname") : NULL)) {
    ASSERT_OR_PRINT_NODE(!bindc, n);
    Append(wname, overload_ext);
    Append(imname, overload_ext);
    if (!fsymname) {
      // Overloaded functions become fsymname 
      fsymname = fname;
      fname = proxy_name_construct(this->getNSpace(), symname);
    }
    Append(fname, overload_ext);
    generic = true;
  }

  // Add the interface subroutine name to the module scope
  if (add_fsymbol(imname, n) == SWIG_NOWRAP)
    return SWIG_NOWRAP;
  // Add the fortran subroutine name to the module scope
  if (fname && add_fsymbol(fname, n) == SWIG_NOWRAP)
    return SWIG_NOWRAP;

  // Save wrapper names
  Setattr(n, "wrap:name", wname);
  Setattr(n, "wrap:imname", imname);
  if (fname) {
    Setattr(n, "wrap:fname", fname);
  }
  if (fsymname) {
    Setattr(n, "wrap:fsymname", fsymname);
  }

  if (member) {
    // Ignore functions whose name is the same as the parent class
    // TODO: use SWIG scoping functions instead
    String *lower_func = Swig_string_lower(fsymname);
    String *symname_cls = Getattr(this->getCurrentClass(), "sym:name");
    String *lower_cls = Swig_string_lower(symname_cls);
    if (Strcmp(lower_func, lower_cls) == 0) {
      Swig_warning(WARN_FORTRAN_NAME_CONFLICT, input_file, line_number,
                   "Ignoring '%s' due to Fortran name ('%s') conflict with '%s'\n",
                   symname, lower_func, symname_cls);
      return SWIG_NOWRAP;
    }
    Delete(lower_cls);
    Delete(lower_func);
  }

  if (member) {
    if (String *selfname = Getattr(n, "fortran:rename_self")) {
      // Modify the first parameter name so that custom types will match
      // But pre-calculate the original name so that user-facing argument names match
      Parm *first_parm = Getattr(n, "parms");
      ASSERT_OR_PRINT_NODE(first_parm, n);
      this->makeParameterName(n, first_parm, 0);
      Setattr(first_parm, "name", selfname);
    }
  }

  // >>> GENERATE WRAPPER CODE

  if (!bindc) {
    // Typical function wrapping: generate C, interface, and proxy wrappers.
    // If something fails, error out early.
    if (this->cfuncWrapper(n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
    if (this->imfuncWrapper(n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
    if (this->proxyfuncWrapper(n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
  } else {
    // C-bound function: set up bindc-type paramneters
    if (this->bindcfuncWrapper(n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
    if (this->imfuncWrapper(n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
  }

  // >>> GENERATE CODE FOR MODULE INTERFACE

  if (GetFlag(n, "fortran:private")) {
    // Hidden function (currently, only constructors that become module procedures)
  } else if (member) {
    // Wrapping a member function
    ASSERT_OR_PRINT_NODE(!this->is_bindc_struct(), n);
    ASSERT_OR_PRINT_NODE(f_class, n);
    ASSERT_OR_PRINT_NODE(fname, n);
    ASSERT_OR_PRINT_NODE(fsymname, n);

    String *qualifiers = NewStringEmpty();

    if (generic) {
      Append(qualifiers, ", private");
    }
    if (String *extra_quals = Getattr(n, "fortran:procedure")) {
      Printv(qualifiers, ", ", extra_quals, NULL);
    }
      
    Printv(f_class, "  procedure", qualifiers, " :: ", NULL);
    
    if (!generic) {
      // Declare procedure name, aliasing the private mangled function name
      // Add qualifiers like "static" for static functions
      Printv(f_class, fsymname, " => ", fname, "\n", NULL);
    } else {
      // Add name to method overload list
      List *overloads = get_default_list(d_method_overloads, fsymname);
      Append(overloads, fname);

      // Declare a private procedure
      Printv(f_class, fname, "\n", NULL);
    }
  } else if (fsymname) {
    // The module function name is aliased, and perhaps overloaded.
    // Append this function name to the list of overloaded names
    // for the symbol. The 'public' access specification gets added later.
    List *overloads = get_default_list(d_overloads, fsymname);
    Append(overloads, fname);
  } else if (bindc) {
    // Expose the interface function 
    ASSERT_OR_PRINT_NODE(imname && Len(imname) > 0, n);
    Printv(f_fdecl, " public :: ", imname, "\n", NULL);
  } else {
    // Expose the proxy function 
    ASSERT_OR_PRINT_NODE(fname && Len(fname) > 0, n);
    Printv(f_fdecl, " public :: ", fname, "\n", NULL);
  }

  Delete(fname);
  Delete(imname);
  Delete(wname);
  Delete(fsymname);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Generate C/C++ wrapping code
 */
int FORTRAN::cfuncWrapper(Node *n) {
  String *symname = Getattr(n, "sym:name");

  Wrapper *cfunc = NewWrapper();

  // >>> RETURN VALUES

  // Get the SWIG type representation of the C return type, but first the
  // ctype typemap has to be attached
  Swig_typemap_lookup("ctype", n, Getattr(n, "name"), NULL);
  SwigType *c_return_type = parse_typemap("ctype", n, WARN_FORTRAN_TYPEMAP_CTYPE_UNDEF);
  if (!c_return_type) {
    Swig_error(input_file, line_number,
               "Failed to parse 'ctype' typemap return value of '%s'\n",
               symname);
    return SWIG_NOWRAP;
  }
  const bool is_csubroutine = (Strcmp(c_return_type, "void") == 0);

  String *c_return_str = NULL;
  if (return_type_needs_typedef(c_return_type)) {
    // For these types (where the name is the middle of the expression rather than at the right side,
    // i.e. void (*func)() instead of int func, we either have to add a new typedef OR wrap the
    // entire function in parens. The former is easier.
    c_return_str = NewStringf("%s_swigrtype", symname);

    String *typedef_str = SwigType_str(c_return_type, c_return_str);
    Printv(cfunc->def, "typedef ", typedef_str, ";\n", NULL);
    Delete(typedef_str);
  } else {
    // Typical case: convert return type into a regular string
    c_return_str = SwigType_str(c_return_type, NULL);
  }

  Printv(cfunc->def, "SWIGEXPORT ", c_return_str, " ", Getattr(n, "wrap:name"), "(", NULL);

  if (!is_csubroutine) {
    // Add local variables for result
    Wrapper_add_localv(cfunc, "fresult", c_return_str, "fresult", NULL);
  }

  // >>> FUNCTION PARAMETERS/ARGUMENTS

  // Emit all of the local variables for holding arguments.
  ParmList *parmlist = Getattr(n, "parms");
  emit_parameter_variables(parmlist, cfunc);
  Swig_typemap_attach_parms("ctype", parmlist, cfunc);
  emit_attach_parmmaps(parmlist, cfunc);
  emit_mark_varargs(parmlist);
  Setattr(n, "wrap:parms", parmlist);

  if (Getattr(n, "sym:overloaded")) {
    // After emitting parameters, check for invalid overloads
    Swig_overload_check(n);
    if (Getattr(n, "overload:ignore")) {
      DelWrapper(cfunc);
      return SWIG_NOWRAP;
    }
  }

  // Create a list of parameters wrapped by the intermediate function
  List *cparmlist = NewList();

  // Loop using the 'tmap:in:next' property rather than 'nextSibling' to account for multi-argument typemaps
  const char *prepend_comma = "";
  for (Parm *p = parmlist; p; p = Getattr(p, "tmap:in:next")) {
    if (checkAttribute(p, "tmap:in:numinputs", "0")) {
      // The typemap is being skipped with the 'numinputs=0' keyword
      continue;
    }
    if (checkAttribute(p, "varargs:ignore", "1")) {
      // We don't understand varargs
      Swig_warning(WARN_LANG_NATIVE_UNIMPL, Getfile(p), Getline(p),
                   "Variable arguments (in function '%s') are not implemented in Fortran.\n",
                   Getattr(n, "sym:name"));
      continue;
    }

    // Name of the argument in the function call (e.g. farg1)
    String *imname = NewStringf("f%s", Getattr(p, "lname"));
    Setattr(p, "imname", imname);
    Append(cparmlist, p);

    // Get the user-provided C type string, and convert it to a SWIG
    // internal representation using Swig_cparse_type . Then convert the
    // type and argument name to a valid C expression using SwigType_str.
    SwigType *parsed_tm = parse_typemap("ctype", "in", p, WARN_FORTRAN_TYPEMAP_CTYPE_UNDEF);
    if (!parsed_tm) {
      Swig_error(input_file, line_number,
                 "Failed to parse 'ctype' typemap for argument '%s' of '%s'\n",
                 SwigType_str(Getattr(p, "type"), Getattr(p, "name")), symname);
      return SWIG_NOWRAP;
    }
    String *carg = SwigType_str(parsed_tm, imname);
    Printv(cfunc->def, prepend_comma, carg, NULL);
    Delete(carg);

    // Since we successfully output an argument, the next one should have a comma before it
    prepend_comma = ", ";
  }

  // Save list of wrapped parms for im declaration and proxy
  Setattr(n, "wrap:cparms", cparmlist);

  // END FUNCTION DEFINITION
  Printv(cfunc->def, ") {", NULL);

  // >>> ADDITIONAL WRAPPER CODE

  String *cleanup = NewStringEmpty();
  String *outarg = NewStringEmpty();

  // Insert input conversion, constraint checking, and cleanup code
  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;
    if (String *tm = Getattr(p, "tmap:in")) {
      this->replace_fclassname(Getattr(p, "type"), tm);
      String *imname = Getattr(p, "imname");
      Replaceall(tm, "$input", imname);
      Setattr(p, "emit:input", imname);
      Printv(cfunc->code, tm, "\n", NULL);
    }
    if (String *tm = Getattr(p, "tmap:check")) {
      Replaceall(tm, "$input", Getattr(p, "emit:input"));
      Printv(cfunc->code, tm, "\n", NULL);
    }
    if (String *tm = Getattr(p, "tmap:freearg")) {
      Replaceall(tm, "$input", Getattr(p, "emit:input"));
      Printv(cleanup, tm, "\n", NULL);
    }
    if (String *tm = Getattr(p, "tmap:argout")) {
      Replaceall(tm, "$result", "fresult");
      Replaceall(tm, "$input", Getattr(p, "emit:input"));
      Printv(outarg, tm, "\n", NULL);
    }
  }

  // Generate code to make the C++ function call
  Swig_director_emit_dynamic_cast(n, cfunc);
  String *actioncode = emit_action(n);

  // Generate code to return the value
  String *return_cpptype = Getattr(n, "type");
  if (String *code = Swig_typemap_lookup_out("out", n, Swig_cresult_name(), cfunc, actioncode)) {
    if (Len(code) > 0) {
      // Output typemap is defined; emit the function call and result
      // conversion code
      Replaceall(code, "$result", "fresult");
      Replaceall(code, "$owner", (GetFlag(n, "feature:new") ? "1" : "0"));
      Printv(cfunc->code, code, "\n", NULL);
    }
  } else {
    // XXX this should probably raise an error
    Swig_warning(WARN_TYPEMAP_OUT_UNDEF, input_file, line_number,
                 "Unable to use return type %s in function %s.\n",
                 SwigType_str(return_cpptype, 0), Getattr(n, "name"));
  }
  emit_return_variable(n, return_cpptype, cfunc);

  // Output argument output and cleanup code
  Printv(cfunc->code, outarg, NULL);
  Printv(cfunc->code, cleanup, NULL);

  // Return value "resource management", as opposed to the "out" typemap's
  // "value conversion" (not used in any of SWIG codebase as far as I can
  // tell; only mentioned once in manual)
  if (String *ret_code = Swig_typemap_lookup("ret", n, Swig_cresult_name(), NULL)) {
    Chop(ret_code);
    Printv(cfunc->code, ret_code, "\n", NULL);
  }

  if (!is_csubroutine) {
    String *qualified_return = SwigType_rcaststr(c_return_str, "fresult");
    Printf(cfunc->code, "    return %s;\n", qualified_return);
    Delete(qualified_return);
  }

  Printf(cfunc->code, "}\n");

  if (Getattr(n, "feature:contract")) {
    // Update contract assertion macro to include the neded return function
    Replaceall(cfunc->code, "SWIG_contract_assert(", "SWIG_contract_assert(return $null, ");
  }

  // Apply standard SWIG substitutions
  if (Strstr(cfunc->code, "$")) {
    // Cleanup code if a function exits early -- in practice, not used.
    Replaceall(cfunc->code, "$cleanup", cleanup);
    // Function name for error messages
    if (Strstr(cfunc->code, "$decl")) {
      // Full function name
      String *decl = Swig_name_decl(n);
      Replaceall(cfunc->code, "$decl", decl);
      Delete(decl);
    }

    // Get 'null' return type if specified
    String *null_return_type = Getattr(n, "tmap:ctype:null");
    Replaceall(cfunc->code, "$null", null_return_type ? null_return_type : "0");

    // Apply standard SWIG substitutions
    Replaceall(cfunc->code, "$symname", Getattr(n, "sym:name"));
  }

  // Write the C++ function into the wrapper code file
  Wrapper_print(cfunc, f_wrapper);

  Delete(cparmlist);
  Delete(outarg);
  Delete(cleanup);
  Delete(c_return_str);
  DelWrapper(cfunc);
  return SWIG_OK;
}


/* -------------------------------------------------------------------------
 * \brief Generate Fortran interface code
 *
 * This is the Fortran equivalent of the cfuncWrapper's declaration.
 */
int FORTRAN::bindcfuncWrapper(Node *n) {
  // Simply binding a function for Fortran
  if (CPlusPlus && !Swig_storage_isexternc(n)) {
    Swig_warning(WARN_LANG_IDENTIFIER, input_file, line_number,
                 "The function '%s' appears not to be defined with external "
                 "C linkage (extern \"C\"). Link errors may result.\n",
                 Getattr(n, "sym:name"));
  }

  // Emit all of the local variables for holding arguments.
  ParmList *parmlist = Getattr(n, "parms");
  Swig_typemap_attach_parms("bindc", parmlist, NULL);
  emit_attach_parmmaps(parmlist, NULL);
  Setattr(n, "wrap:parms", parmlist);

  // Create a list of parameters wrapped by the intermediate function
  List *cparmlist = NewList();
  int i = 0;
  for (Parm *p = parmlist; p; p = nextSibling(p), ++i) {
    // Check for varargs
    if (SwigType_isvarargs(Getattr(p, "type"))) {
      Swig_warning(WARN_LANG_NATIVE_UNIMPL, Getfile(p), Getline(p),
                   "C-bound variable arguments (in function '%s') are not implemented in Fortran.\n",
                   Getattr(n, "sym:name"));
      return SWIG_NOWRAP;
    }
    // Use C arguments
    String *imname = this->makeParameterName(n, p, i);
    Setattr(p, "imname", imname);
    Append(cparmlist, p);
  }

  // Save list of wrapped parms for im declaration and proxy
  Setattr(n, "wrap:cparms", cparmlist);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Generate Fortran interface code
 *
 * This is the Fortran equivalent of the cfuncWrapper's declaration.
 */
int FORTRAN::imfuncWrapper(Node *n) {
  Wrapper *imfunc = NewFortranWrapper();

  const char *tmtype = "imtype";
  int warning_flag = WARN_FORTRAN_TYPEMAP_IMTYPE_UNDEF;
  if (is_bindc(n)) {
    tmtype = "bindc";
    warning_flag = WARN_TYPEMAP_UNDEF;
  }

  // >>> RETURN VALUES

  String *return_cpptype = Getattr(n, "type");

  // Attach typemap for return value
  String *return_imtype = attach_typemap(tmtype, n, warning_flag);
  this->replace_fclassname(return_cpptype, return_imtype);

  const bool is_imsubroutine = (Len(return_imtype) == 0);

  // Determine based on return typemap whether it's a function or subroutine (we could equivalently check that return_cpptype is `void`)
  const char *im_func_type = (is_imsubroutine ? "subroutine" : "function");
  Printv(imfunc->def, im_func_type, " ", Getattr(n, "wrap:imname"), "(", NULL);

  // Hash of import statements needed for the interface code
  Hash *imimport_hash = NewHash();

  // If return type is a fortran C-bound type, add import statement
  if (String *imimport = make_import_string(return_imtype)) {
    SetFlag(imimport_hash, imimport);
    Delete(imimport);
  }

  // >>> FUNCTION PARAMETERS/ARGUMENTS

  ParmList *parmlist = Getattr(n, "parms");
  Swig_typemap_attach_parms(tmtype, parmlist, NULL);

  // Get the list of actual parameters used by the C function
  // (these are pointers to values in parmlist, with some elements possibly
  // removed)
  List *cparmlist = Getattr(n, "wrap:cparms");
  assert(cparmlist);

  // Append "using" statements and dummy variables to the interface
  // "definition" (before the code and local variable declarations)
  String *imlocals = NewStringEmpty();

  // >>> BUILD WRAPPER FUNCTION AND INTERFACE CODE
  List *imfunc_arglist = NewList();
  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;

    // Add function parameter name (e.g. farg1) to the arglist
    String *imname = Getattr(p, "imname");
    Append(imfunc_arglist, imname);

    // Add dummy argument to wrapper body
    String *imtype = get_typemap(tmtype, "in", p, warning_flag);
    String *cpptype = Getattr(p, "type");
    this->replace_fclassname(cpptype, imtype);
    Printv(imlocals, "\n   ", imtype, " :: ", imname, NULL);

    // Check for bad dimension parameters
    if (bad_fortran_dims(p, tmtype)) {
      return SWIG_NOWRAP;
    }

    // Include import statements if present; needed for actual structs
    // passed into interface code
    if (String *imimport = make_import_string(imtype)) {
      SetFlag(imimport_hash, imimport);
      Delete(imimport);
    }
  }

  // END FUNCTION DEFINITION
  print_wrapped_list(imfunc->def, First(imfunc_arglist), Len(imfunc->def));
  Printv(imfunc->def,
         ") &\n"
         "    bind(C, name=\"",
         Getattr(n, "wrap:name"),
         "\")",
         NULL);

  if (!is_imsubroutine) {
    // Declare dummy return value if it's a function
    Printv(imfunc->def, " &\n     result(fresult)", NULL);
    Printv(imlocals, "\n", return_imtype, " :: fresult", NULL);
  }

  // Write the function local block
  Printv(imfunc->code, "   use, intrinsic :: ISO_C_BINDING", NULL);
  for (Iterator kv = First(imimport_hash); kv.key; kv = Next(kv)) {
    Printv(imfunc->code, "\n   import :: ", kv.key, NULL);
  }
  Printv(imfunc->code, imlocals, "\n  end ", im_func_type, NULL);

  // Write the C++ function into the wrapper code file
  Wrapper_print(imfunc, f_finterfaces);

  DelWrapper(imfunc);
  Delete(imimport_hash);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Generate Fortran proxy code
 *
 * This is for the native Fortran interaction.
 */
int FORTRAN::proxyfuncWrapper(Node *n) {
  Wrapper *ffunc = NewFortranWrapper();

  // Write documentation
  this->write_docstring(n, f_fsubprograms);

  // >>> FUNCTION RETURN VALUES

  String *return_ftype = attach_typemap("ftype", n, WARN_FORTRAN_TYPEMAP_FTYPE_UNDEF);
  assert(return_ftype);

  // Return type for the C call
  String *return_imtype = get_typemap("imtype", n, WARN_NONE);

  // Check whether the Fortran proxy routine returns a variable, and whether
  // the actual C function does

  // Replace any instance of $fclassname in return type
  SwigType *return_cpptype = Getattr(n, "type");
  this->replace_fclassname(return_cpptype, return_ftype);
  this->replace_fclassname(return_cpptype, return_imtype);

  // String for calling the im wrapper on the fortran side (the "action")
  String *fcall = NewStringEmpty();

  const bool is_imsubroutine = (Len(return_imtype) == 0);
  if (!is_imsubroutine) {
    Wrapper_add_localv(ffunc, "fresult", return_imtype, ":: fresult", NULL);
    // Call function and set intermediate result
    Printv(fcall, "fresult = ", NULL);
  } else {
    Printv(fcall, "call ", NULL);
  }
  Printv(fcall, Getattr(n, "wrap:imname"), "(", NULL);

  bool func_to_subroutine = !is_imsubroutine && GetFlag(n, "feature:fortran:subroutine");
  if (func_to_subroutine && GetFlag(n, "tmap:ftype:nofortransubroutine")) {
      Swig_warning(WARN_FORTRAN_NO_SUBROUTINE, Getfile(n), Getline(n),
                   "The given type '%s' cannot be converted from a function result to an optional subroutine argument" ,
                   return_cpptype);
      func_to_subroutine = false;
  }
  const bool is_fsubroutine = (Len(return_ftype) == 0) || func_to_subroutine;

  String *swig_result_name = NULL;
  if (!is_fsubroutine || func_to_subroutine) {
    if (String *fresult_override = Getattr(n, "wrap:fresult")) {
      swig_result_name = fresult_override;
    } else {
      swig_result_name = NewString("swig_result");
    }
  }

  String *fargs = NewStringEmpty();
  if (!is_fsubroutine && !func_to_subroutine) {
    // Add dummy variable for Fortran proxy return
    Printv(fargs, return_ftype, " :: ", swig_result_name, "\n", NULL);
  }

  // >>> FUNCTION NAME

  const char *f_func_type = (is_fsubroutine ? "subroutine" : "function");
  Printv(ffunc->def, f_func_type, " ", Getattr(n, "wrap:fname"), "(", NULL);

  // >>> FUNCTION PARAMETERS/ARGUMENTS

  // Get the list of actual parameters used by the C function
  // (these are pointers to values in parmlist, with some elements possibly
  // removed)
  List *cparmlist = Getattr(n, "wrap:cparms");
  assert(cparmlist);

  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;
    // Temporarily set lname to imname so that "fin" typemap will
    // substitute farg1 instead of arg1
    Setattr(p, "lname:saved", Getattr(p, "lname"));
    Setattr(p, "lname", Getattr(p, "imname"));
  }

  // Attach proxy input typemap (proxy arg -> farg1 in fortran function)
  ParmList *parmlist = Getattr(n, "parms");
  Swig_typemap_attach_parms("ftype", parmlist, ffunc);
  Swig_typemap_attach_parms("fin", parmlist, ffunc);
  Swig_typemap_attach_parms("findecl", parmlist, ffunc);
  Swig_typemap_attach_parms("fargout", parmlist, ffunc);

  // Restore parameter names
  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;
    String *imname = Getattr(p, "imname");

    // Emit local intermediate parameter in the proxy function
    String *imtype = get_typemap("imtype", p, WARN_FORTRAN_TYPEMAP_IMTYPE_UNDEF);
    this->replace_fclassname(Getattr(p, "type"), imtype);
    Wrapper_add_localv(ffunc, imname, imtype, "::", imname, NULL);

    // Restore local variable name
    Setattr(p, "lname", Getattr(p, "lname:saved"));
    Delattr(p, "lname:saved");
  }

  // >>> BUILD WRAPPER FUNCTION AND INTERFACE CODE

  String *prepend = Getattr(n, "feature:fortran:prepend");
  if (prepend) {
    Chop(prepend);
    Printv(ffunc->code, prepend, "\n", NULL);
  }

  int i = 0;
  List *ffunc_arglist = NewList();
  List *fcall_arglist = NewList();
  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;
    String *cpptype = Getattr(p, "type");

    // Add parameter name to declaration list
    String *farg = this->makeParameterName(n, p, i++);
    Append(ffunc_arglist, farg);

    // Add dummy argument to wrapper body
    String *ftype = get_typemap("ftype", "in", p, WARN_FORTRAN_TYPEMAP_FTYPE_UNDEF);
    this->replace_fclassname(cpptype, ftype);
    Printv(fargs, "   ", ftype, " :: ", farg, "\n", NULL);

    if (bad_fortran_dims(p, "ftype")) {
      return SWIG_NOWRAP;
    }

    // Add this argument to the intermediate call function
    Append(fcall_arglist, Getattr(p, "imname"));

    // >>> F PROXY CONVERSION

    String *fin = get_typemap("fin", p, WARN_TYPEMAP_IN_UNDEF);
    if (Len(fin) > 0) {
      Replaceall(fin, "$input", farg);
      Printv(ffunc->code, fin, "\n", NULL);
    }

    // Add any needed temporary variables
    String *findecl = get_typemap("findecl", p, WARN_NONE);
    if (findecl && Len(findecl) > 0) {
      Chop(findecl);
      Printv(fargs, findecl, "\n", NULL);
    }

    Delete(farg);
  }

  if (func_to_subroutine) {
    assert(swig_result_name);
    Append(ffunc_arglist, swig_result_name);

    Printv(fargs, return_ftype, ", intent(out), optional :: ", swig_result_name, "\n", NULL);
  }

  // END FUNCTION DEFINITION
  print_wrapped_list(ffunc->def, First(ffunc_arglist), Len(ffunc->def));
  Printv(ffunc->def, ")", NULL);
  if (!is_fsubroutine) {
    Setattr(n, "fname", swig_result_name);
    Printv(ffunc->def, " &\n     result(", swig_result_name, ")", NULL);
  }
  Delete(ffunc_arglist);
  
  // END FUNCTION DEFINITION
  print_wrapped_list(fcall, First(fcall_arglist), Len(fcall));
  Printv(fcall, ")", NULL);
  Delete(fcall_arglist);

  // Save fortran function call action
  Setattr(n, "wrap:faction", fcall);

  // Emit code to make the Fortran function call in the proxy code
  if (String *action_wrap = Getattr(n, "feature:shadow")) {
    Replaceall(action_wrap, "$action", fcall);
    Chop(action_wrap);
    Printv(ffunc->code, action_wrap, "\n", NULL);
  } else {
    Printv(ffunc->code, fcall, "\n", NULL);
  }

  // Append dummy variables to the proxy function definition
  Chop(fargs);
  Printv(ffunc->def, "\n   use, intrinsic :: ISO_C_BINDING\n", fargs, NULL);

  // >>> ADDITIONAL WRAPPER CODE

  // Get the typemap for output argument conversion
  Parm *temp = NewParm(return_cpptype, Getattr(n, "name"), n);
  Setattr(temp, "lname", "fresult"); // Replaces $1
  String *fbody = attach_typemap("fout", temp, WARN_FORTRAN_TYPEMAP_FOUT_UNDEF);
  if (bad_fortran_dims(temp, "fout")) {
    return SWIG_NOWRAP;
  }

  String *fparm = attach_typemap("foutdecl", temp, WARN_NONE);
  Delete(temp);
  Chop(fbody);

  if (fparm && Len(fparm) > 0) {
    Chop(fparm);
    // Write fortran output parameters after dummy argument
    Printv(ffunc->def, "\n", fparm, NULL);
  }

  // Output typemap is defined; emit the function call and result
  // conversion code
  if (Len(fbody) > 0) {
    if (func_to_subroutine) {
      Insert(fbody, 0, "if (present($result)) then\n");
    }
    Replaceall(fbody, "$result", swig_result_name);
    Replaceall(fbody, "$owner", (GetFlag(n, "feature:new") ? ".true." : ".false."));
    this->replace_fclassname(return_cpptype, fbody);
    if (func_to_subroutine) {
      Printv(fbody, "\nendif\n", NULL);
    }
    Printv(ffunc->code, fbody, "\n", NULL);
  }

  // Add post-call conversion routines for input arguments
  for (Iterator it = First(cparmlist); it.item; it = Next(it)) {
    Parm *p = it.item;
    String *tm = Getattr(p, "tmap:fargout");
    if (tm && Len(tm) > 0) {
      Chop(tm);
      Replaceall(tm, "$result", swig_result_name);
      Replaceall(tm, "$input", Getattr(p, "fname"));
      Replaceall(tm, "$1", Getattr(p, "imname"));
      Printv(ffunc->code, tm, "\n", NULL);
    }
  }

  // Optional "append" proxy code
  String *append = Getattr(n, "feature:fortran:append");
  if (append) {
    Chop(append);
    Printv(ffunc->code, append, "\n", NULL);
  }

  // Output argument output and cleanup code
  Printv(ffunc->code, "  end ", f_func_type, NULL);

  // Write the C++ function into the wrapper code file
  Wrapper_print(ffunc, f_fsubprograms);

  DelWrapper(ffunc);
  Delete(fcall);
  Delete(fargs);
  Delete(swig_result_name);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * Add an assignment operator.
 *
 * The LHS must be intent(inout), and the RHS must be intent(in).
 */
void FORTRAN::add_assignment_operator(Node *classn) {
  ASSERT_OR_PRINT_NODE(Strcmp(nodeType(classn), "class") == 0 && !this->is_bindc_struct(), classn);

  // Create new node representing self-assignment function
  Node *n = NewHash();
  set_nodeType(n, "cdecl");
  Setfile(n, Getfile(classn));
  Setline(n, Getline(classn));

  String *name = NewString("operator =");
  String *symname = NewString("op_assign__");

  Setattr(n, "kind", "function");
  Setattr(n, "name", name);
  Setattr(n, "sym:name", symname);
  Setattr(n, "feature:fortran:generic", "assignment(=)");

  // Add to the class's symbol table
  Symtab *prev_scope = Swig_symbol_setscope(Getattr(classn, "symtab"));
  Node *added = Swig_symbol_add(symname, n);
  Swig_symbol_setscope(prev_scope);
  ASSERT_OR_PRINT_NODE(added == n, n);

  // Make sure the function declaration is public
  Setattr(n, "access", "public");

  // Function declaration: takes const reference to class, returns nothing
  SwigType *classtype = Getattr(classn, "classtypeobj");
  String *decl = NewStringf("f(r.q(const).%s).", classtype);
  Setattr(n, "decl", decl);
  Setattr(n, "type", "void");
  
  // Change parameters so that the correct self/other are used for typemap matching.
  // Notably, 'other' should be treated as a *MUTABLE* reference for type matching.
  String *argtype = NewStringf("r.%s", classtype);
  Parm *other_parm = NewParm(argtype, "other", classn);
  this->makeParameterName(n, other_parm, 0);
  Setattr(other_parm, "name", "ASSIGNMENT_OTHER");
  Setattr(n, "parms", other_parm);
  Setattr(n, "fortran:rename_self", "ASSIGNMENT_SELF"); // Use INOUT for class handle

  // Get C++ class name
  String *classname = Getattr(classn, "classtype");
  if (String *smartptr_type = Getattr(classn, "feature:smartptr")) {
    // The pointed-to data is actually SP<CLASS>, not CLASS.
    classname = smartptr_type;
  }
  // Determine construction flags.
  String *policystr = Getattr(classn, "fortran:policy");

  // Define action code
  String *code = NULL;
  if (CPlusPlus) {
    code = NewStringf("SWIG_assign<%s, %s>(farg1, *farg2);\n", classname, policystr);
  } else {
    code = NewStringf("SWIG_assign(farg1, *farg2);\n");
  }
  Setattr(n, "feature:action", code);

  // Insert assignment fragment
  Setattr(n, "feature:fragment", "SWIG_assign");

  // Add the new assignment operator to the class's definition.
  appendChild(classn, n);
  
  Delete(code);
  Delete(classtype);
  Delete(other_parm);
  Delete(symname);
  Delete(name);
  Delete(decl);
  Delete(argtype);
}

/* -------------------------------------------------------------------------
 * \brief Write documentation for the given node to the passed string.
 */
void FORTRAN::write_docstring(Node *n, String *dest) {
  String *docs = Getattr(n, "feature:docstring");

  if (!docs)
    return;

  List *lines = SplitLines(docs);

  // Skip leading blank lines
  Iterator it = First(lines);
  while (it.item && Len(it.item) == 0) {
    it = Next(it);
  }

  for (; it.item; it = Next(it)) {
    // Chop(it.item);
    Printv(dest, "! ", it.item, "\n", NULL);
  }

  Delete(lines);
}

/* -------------------------------------------------------------------------
 * \brief Create a friendly parameter name
 */
String *FORTRAN::makeParameterName(Node *n, Parm *p, int arg_num, bool) const {
  String *name = Getattr(p, "fname");
  if (name) {
    // Name has already been converted and checked by a previous loop
    return name;
  }

  name = Getattr(p, "name");
  if (name && Len(name) > 0 && is_valid_identifier(name) && !Strstr(name, "::")) {
    // Valid fortran name; convert to lowercase
    name = Swig_string_lower(name);
  } else {
    // Invalid name; replace with something simple
    name = NewStringf("arg%d", arg_num);
  }
  String *origname = name;

  // Symbol tables for module and forward-declared class scopes
  FORTRAN *mthis = const_cast<FORTRAN *>(this);
  Hash *symtab = mthis->symbolScopeLookup("fortran");
  
  bool valid = false;
  while (!valid)
  {
    valid = true;
    if (ParmList *parmlist = Getattr(n, "parms")) {
      // Check against previously generated names in this parameter list
      for (Parm *other = parmlist; other; other = nextSibling(other)) {
        if (other == p)
          break;
        String *other_name = Getattr(other, "fname");
        if (other_name && Strcmp(name, other_name) == 0) {
          valid = false;
          break;
        }
      }
    }

    // If the parameter name is in the fortran scope, or in the
    // forward-declared classes, mangle it
    if (valid && (Getattr(symtab, name))) {
      valid = false;
    }

    if (!valid) {
      if (name != origname)
        Delete(name);
      // Try another name and loop again
      name = NewStringf("%s%d", origname, arg_num++);
    }
  }

  // Save the name for next time we have to use this parameter
  Setattr(p, "fname", name);
  return name;
}

/* -------------------------------------------------------------------------
 * \brief Process a class declaration.
 *
 * The superclass calls classHandler.
 */
int FORTRAN::classDeclaration(Node *n) {
  if (!GetFlag(n, "feature:onlychildren")) {
    // Create unique name and add to symbol table
    if (!Getattr(n, "fortran:name")) {
      String *fsymname = this->make_unique_symname(n);
      Setattr(n, "fortran:name", fsymname);
      Delete(fsymname);
    }
  }
  if (is_bindc(n)) {
    // Prevent default constructors, destructors, etc.
    SetFlag(n, "feature:nodefault");
  } 
  return Language::classDeclaration(n);
}

/* -------------------------------------------------------------------------
 * \brief Process classes.
 */
int FORTRAN::classHandler(Node *n) {
  // Add the class name or warn if it's a duplicate
  String *symname = Getattr(n, "fortran:name");
  ASSERT_OR_PRINT_NODE(symname, n);
  String *basename = NULL;

  // Iterate through the base classes. If no bases are set (null pointer sent
  // to `First`), the loop will be skipped and baseclass be NULL.
  for (Iterator base = First(Getattr(n, "bases")); base.item; base = Next(base)) {
    Node *b = base.item;
    if (GetFlag(b, "feature:ignore"))
      continue;
    if (!basename) {
      // First class that was encountered
      basename = Getattr(b, "fortran:name");
    } else {
      // Another base class exists
      Swig_warning(WARN_FORTRAN_MULTIPLE_INHERITANCE, Getfile(n), Getline(n),
                   "Multiple inheritance is not supported in Fortran. Ignoring base class %s for %s\n",
                   Getattr(b, "sym:name"),
                   Getattr(n, "sym:name"));
    }
  }

  const bool bindc = is_bindc(n);
  if (bindc && basename) {
    // Disallow inheritance for BIND(C) types
    Swig_error(input_file, line_number,
               "Struct '%s' uses the '%%fortran_bindc_struct' feature, so it cannot use inheritance.\n",
               symname);
    return SWIG_NOWRAP;
  }

  ASSERT_OR_PRINT_NODE(!f_class, n);
  f_class = NewStringEmpty();

  ASSERT_OR_PRINT_NODE(Getattr(n, "kind") && Getattr(n, "classtype"), n);
  f_class = NewStringf(" ! %s %s\n", Getattr(n, "kind"), Getattr(n, "classtype"));
  
  // Write documentation
  this->write_docstring(n, f_class);

  // Declare class
  Printv(f_class, " type", NULL);
  if (basename) {
    Printv(f_class, ", extends(", basename, ")", NULL);
  } else if (bindc) {
    Printv(f_class, ", bind(C)", NULL);
  }
  Printv(f_class, ", public :: ", symname, "\n", NULL);

  // Define policy
  if (CPlusPlus)
  {
    SwigType *name = Getattr(n, "name");
    ASSERT_OR_PRINT_NODE(name, n);
    String *policystr = SwigType_manglestr(name);
    Insert(policystr, 0, "SWIGPOLICY");
    Setattr(n, "fortran:policy", policystr);

    // Define policies for the class
    const char *policy = "swig::ASSIGNMENT_DEFAULT";
    if (String *smartptr_type = Getattr(n, "feature:smartptr")) {
      policy = "swig::ASSIGNMENT_SMARTPTR";
    } else if (!GetFlag(n, "allocate:default_destructor")) {
      policy = "swig::ASSIGNMENT_NODESTRUCT";
    }
    Printv(f_policies, "#define ", policystr, " ", policy, "\n", NULL);
  }

  int saved_classlen = 0;
  if (!bindc) {
    if (!basename) {
      // Insert the class data if this doesn't inherit from anything
      emit_fragment("SwigClassWrapper_f");
      Printv(f_class,
             "  type(SwigClassWrapper), public :: swigdata\n",
             NULL);
    }

    // Initialize output strings that will be added by 'functionHandler'.
    d_method_overloads = NewHash();

    // Constructors
    d_constructors = NewList();

    // Add an assignment function to the class node
    this->add_assignment_operator(n);

    Printv(f_class, " contains\n", NULL);
    saved_classlen = Len(f_class);
  }

  // Emit class members
  Language::classHandler(n);

  if (!bindc) {
    // Write overloads
    for (Iterator kv = First(d_method_overloads); kv.key; kv = Next(kv)) {
      Printv(f_class, "  generic :: ", kv.key, " => ", NULL);
      // Note: subtract 2 becaues this first line is an exception to
      // prepend_comma, added inside the iterator
      int line_length = 13 + Len(kv.key) + 4 - 2;

      // Write overloaded procedure names
      print_wrapped_list(f_class, First(kv.item), line_length);
      Printv(f_class, "\n", NULL);
    }
  }

  // Close out the type
  Printf(f_class, " end type %s\n", symname);

  // Save overloads as a node attribute for debugging
  if (d_method_overloads) {
    Setattr(n, "fortran:overloads", d_method_overloads);
    Delete(d_method_overloads);
    d_method_overloads = NULL;
  }

  // Write the constructed class out to the declaration part of the module
  Printv(f_fdecl, f_class, NULL);
  Delete(f_class);
  f_class = NULL;

  // Print constructor interfaces
  if (d_constructors && (Len(d_constructors) > 0)) {
    Printf(f_fdecl, " interface %s\n", symname);
    for (Iterator it = First(d_constructors); it.item; it = Next(it)) {
      Printf(f_fdecl, "  module procedure %s\n", it.item);
    }
    Printf(f_fdecl, " end interface\n");
    Setattr(n, "fortran:constructors", d_constructors);
    Delete(d_constructors);
    d_constructors = NULL;
  }

  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Extra stuff for constructors.
 */
int FORTRAN::constructorHandler(Node *n) {
  // Add swigf_ to constructor name
  String *fname = proxy_name_construct(this->getNSpace(), "create", Getattr(n, "sym:name"));
  Setattr(n, "fortran:fname", fname);
  Delete(fname);

  // Override the result variable name
  Setattr(n, "wrap:fresult", "self");
  // Don't generate a public interface
  SetFlag(n, "fortran:private");

  Language::constructorHandler(n);

  Append(d_constructors, Getattr(n, "wrap:fname"));
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Handle extra destructor stuff.
 */
int FORTRAN::destructorHandler(Node *n) {
  // Make the destructor a member function called 'release'
  Setattr(n, "fortran:name", "release");
  SetFlag(n, "fortran:ismember");

  // Add swigf_ to constructor name
  String *fname = proxy_name_construct(this->getNSpace(), "release", Getattr(n, "sym:name"));
  Setattr(n, "fortran:fname", fname);
  Delete(fname);

  // Use a custom typemap: input must be mutable and clean up properly
  Setattr(n, "fortran:rename_self", "DESTRUCTOR_SELF");
  // Wrap the proxy action so it only 'delete's if it owns
  Setattr(n, "feature:shadow",
          "if (btest(farg1%cmemflags, swig_cmem_own_bit)) then\n"
          "  $action\n"
          "endif\n"
          "farg1%cptr = C_NULL_PTR\n"
          "farg1%cmemflags = 0\n");

  return Language::destructorHandler(n);
}

/* -------------------------------------------------------------------------
 * \brief Process member functions.
 *
 * This is *NOT* called when generating get/set wrappers for membervariableHandler.
 */
int FORTRAN::memberfunctionHandler(Node *n) {
  String *class_symname = Getattr(this->getCurrentClass(), "sym:name");

  if (this->is_bindc_struct()) {
    Swig_error(input_file, line_number,
               "Struct '%s' has the 'fortranbindc' feature set, so it cannot have member functions\n",
               class_symname);
    return SWIG_NOWRAP;
  }

  // Create a private procedure name that gets bound to the Fortan TYPE
  String *fwrapname = proxy_name_construct(this->getNSpace(), class_symname,Getattr(n, "sym:name"));
  Setattr(n, "fortran:fname", fwrapname);
  Delete(fwrapname);

  // Save original member function name, mangled to a valid fortran name
  Setattr(n, "fortran:name", make_fname(Getattr(n, "sym:name")));

  // Set as a member variable unless it's a constructor
  if (!is_node_constructor(n)) {
    SetFlag(n, "fortran:ismember");
  }

  Language::memberfunctionHandler(n);

  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Process member variables.
 */
int FORTRAN::membervariableHandler(Node *n) {
  String *fsymname = make_fname(Getattr(n, "sym:name"));
  if (this->is_bindc_struct()) {
    // Write the type for the class member
    String *bindc_typestr = attach_typemap("bindc", n, WARN_TYPEMAP_UNDEF);
    SwigType *datatype = Getattr(n, "type");

    if (!bindc_typestr) {
      // In order for the struct's data to correspond to the C-aligned
      // data, an interface type MUST be specified!
      String *class_symname = Getattr(this->getCurrentClass(), "sym:name");
      Swig_error(input_file, line_number,
                 "Struct '%s' has the 'bindc' feature set, but member variable '%s' (type '%s') has no 'bindc' typemap defined\n",
                 class_symname, fsymname, SwigType_namestr(datatype));
      return SWIG_NOWRAP;
    }
    this->replace_fclassname(datatype, bindc_typestr);

    ASSERT_OR_PRINT_NODE(Len(fsymname) > 0, n);
    Printv(f_class, "  ", bindc_typestr, ", public :: ", fsymname, "\n", NULL);
    Delete(fsymname);
  } else {
    // Create getter and/or setter functions, first preserving
    // the original member variable name
    Setattr(n, "fortran:variable", fsymname);
    SetFlag(n, "fortran:ismember");
    Language::membervariableHandler(n);
  }
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Process static member functions.
 */
int FORTRAN::globalvariableHandler(Node *n) {
  if (GetFlag(n, "feature:fortran:const")) {
    this->constantWrapper(n);
  } else if (is_bindc(n)) {
    Swig_error(input_file, line_number,
               "Can't wrap '%s': %%fortranbindc support for global variables is not yet implemented\n",
               Getattr(n, "sym:name"));
  } else {
    String *fsymname = Copy(Getattr(n, "sym:name"));
    Setattr(n, "fortran:variable", fsymname);
    Language::globalvariableHandler(n);
    Delete(fsymname);
  }
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Process static member functions.
 */
int FORTRAN::staticmemberfunctionHandler(Node *n) {
  String *class_symname = Getattr(getCurrentClass(), "sym:name");
  if (this->is_bindc_struct()) {
    Swig_error(input_file, line_number,
               "Struct '%s' has the 'fortranbindc' feature set, so it cannot have static member functions\n",
               class_symname);
    return SWIG_NOWRAP;
  }

  // Preserve original function name
  Setattr(n, "fortran:name", make_fname(Getattr(n, "sym:name")));

  // Create a private procedure name that gets bound to the Fortan TYPE
  String *fwrapname = proxy_name_construct(this->getNSpace(), class_symname,Getattr(n, "sym:name"));
  
  Setattr(n, "fortran:fname", fwrapname);

  // Add 'nopass' procedure qualifier
  Setattr(n, "fortran:procedure", "nopass");

  // Mark as a member function
  SetFlag(n, "fortran:ismember");

  Language::staticmemberfunctionHandler(n);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Process static member variables.
 */
int FORTRAN::staticmembervariableHandler(Node *n) {
  // Preserve variable name
  Setattr(n, "fortran:variable", Getattr(n, "sym:name"));

  SetFlag(n, "fortran:ismember");

  // Add 'nopass' procedure qualifier for getters and setters
  Setattr(n, "fortran:procedure", "nopass");
  Language::staticmembervariableHandler(n);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Wrap an enum declaration
 */
int FORTRAN::enumDeclaration(Node *n) {
  String *access = Getattr(n, "access");
  if (access && Strcmp(access, "public") != 0) {
    // Not a public enum
    return SWIG_NOWRAP;
  }

  if (GetFlag(n, "sym:weak")) {
    // Ignore forward declarations
    return SWIG_NOWRAP;
  }

  String *enum_name = NULL;
  String *symname = Getattr(n, "sym:name");
  if (!symname) {
    // Anonymous enum TYPE:
    // enum {FOO=0, BAR=1};
  } else if (Strstr(symname, "$unnamed") != NULL) {
    // Anonymous enum VALUE
    // enum {FOO=0, BAR=1} foo;
  } else if (Node *classnode = this->getCurrentClass()) {
    // Scope the enum since it's in a class
    String *tempname = NewStringf("%s_%s", Getattr(classnode, "sym:name"), symname);
    enum_name = make_fname(tempname);
    Delete(tempname);
    // Save the alias name
    Setattr(n, "fortran:name", enum_name);
    // Add to symbol table
    if (add_fsymbol(enum_name, n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
  } else if (String *fortranname = Getattr(n, "fortran:name")) {
    enum_name = Copy(fortranname);
  } else {
    enum_name = make_fname(symname);
    if (add_fsymbol(enum_name, n) == SWIG_NOWRAP)
      return SWIG_NOWRAP;
  }

  if (ImportMode) {
    // Don't generate wrappers if we're in import mode, but make sure the symbol renaming above is still performed. Also make sure to mark that the enum is available for use as a type
    SetFlag(n, "fortran:declared");
    return SWIG_OK;
  }

  if (String *name = Getattr(n, "name")) {
    Printv(f_fdecl, " ! ", NULL);
    if (String *storage = Getattr(n, "storage")) {
      Printv(f_fdecl, storage, " ", NULL);
    }
    Printv(f_fdecl, Getattr(n, "enumkey"), " ", name, "\n", NULL);
  }

  // Determine whether to add enum as a native fortran enumeration. If false,
  // the values are all wrapped as constants. Only create the list if values are defined.
  if (is_native_enum(n) && firstChild(n)) {
    // Create enumerator statement and initialize list of enum values
    d_enum_public = NewList();
    Printv(f_fdecl, " enum, bind(c)\n", NULL);

    // Mark that the enum is available for use as a type
    SetFlag(n, "fortran:declared");
  }

  // Emit enum items
  Language::enumDeclaration(n);

  if (d_enum_public) {
    ASSERT_OR_PRINT_NODE(Len(d_enum_public) > 0, n);
    // End enumeration
    Printv(f_fdecl, " end enum\n", NULL);

    if (enum_name) {
      ASSERT_OR_PRINT_NODE(Len(enum_name) > 0, n);
      // Create "kind=" value for the enumeration type
      Printv(f_fdecl, " integer, parameter, public :: ",  enum_name,
             " = kind(", First(d_enum_public).item, ")\n", NULL);
    }

    // Make the enum values public
    Printv(f_fdecl, " public :: ", NULL);
    print_wrapped_list(f_fdecl, First(d_enum_public), 11);
    Putc('\n', f_fdecl);

    // Clean up
    Delete(d_enum_public);
    d_enum_public = NULL;
  } else if (enum_name) {
    // Create "kind=" value for the enumeration type
    Printv(f_fdecl, " integer, parameter, public :: ",  enum_name,
           " = C_INT\n", NULL);

    // Mark that the enum is available for use as a type
    SetFlag(n, "fortran:declared");
  }

  // Clean up
  Delete(enum_name);

  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Process constants
 *
 * These include callbacks declared with

     %constant int (*ADD)(int,int) = add;

 * as well as values such as

     %constant int wrapped_const = (1 << 3) | 1;
     #define MY_INT 0x123

 * that need to be interpreted by the C compiler.
 *
 * They're also called inside enumvalueDeclaration (either directly or through
 * memberconstantHandler)
 */
int FORTRAN::constantWrapper(Node *n) {
  String *nodetype = nodeType(n);
  String *symname = Getattr(n, "sym:name");
  String *value = Getattr(n, "rawval");

  if (String *override_value = Getattr(n, "feature:fortran:constvalue")) {
    value = override_value;
    Setattr(n, "feature:fortran:const", "1");
  }

  if (Strcmp(nodetype, "enumitem") == 0) {
    // Make unique enum values for the user
    symname = this->make_unique_symname(n);

    // Set type from the parent enumeration
    String *t = Getattr(parentNode(n), "enumtype");
    Setattr(n, "type", t);

    if (!value) {
      if (d_enum_public) {
        // We are wrapping an enumeration in Fortran. Get the enum value if
        // present; if not, Fortran enums take the same value as C enums.
        value = Getattr(n, "enumvalue");
      } else {
        // Wrapping as a constant
        value = Getattr(n, "value");
      }
    }
  } else if (Strcmp(nodetype, "enum") == 0) {
    // Symbolic name is already unique
    ASSERT_OR_PRINT_NODE(!value, n);
    // But we're wrapping the enumeration type as a fictional value
    value = Getattr(n, "value");
  } else {
    // Make unique enum values for the user
    symname = this->make_unique_symname(n);
    if (!value) {
      value = Getattr(n, "value");
    }
  }

  ASSERT_OR_PRINT_NODE(value || d_enum_public, n);

  // Get Fortran data type
  String *bindc_typestr = attach_typemap("bindc", n, WARN_NONE);
  if (!bindc_typestr) {
    Swig_warning(WARN_TYPEMAP_UNDEF, Getfile(n), Getline(n),
                 "The 'bindc' typemap for '%s' is not defined, so the corresponding constant cannot be generated\n",
                 SwigType_str(Getattr(n, "type"), Getattr(n, "sym:name")));
    return SWIG_NOWRAP;
  }

  // Check for incompatible array dimensions
  if (bad_fortran_dims(n, "bindc")) {
    return SWIG_NOWRAP;
  }

  if (d_enum_public) {
    ASSERT_OR_PRINT_NODE(Len(symname) > 0, n);
    // We're wrapping a native enumerator: add to the list of enums being
    // built
    Append(d_enum_public, symname);
    // Print the enum to the list
    Printv(f_fdecl, "  enumerator :: ", symname, NULL);
    if (value) {
      Printv(f_fdecl, " = ", value, NULL);
    }
    Printv(f_fdecl, "\n", NULL);
  } else if (is_native_parameter(n)) {
    String *suffix = make_specifier_suffix(bindc_typestr);
    if (suffix) {
      // Add specifier such as _C_DOUBLE to the value. Otherwise, for example,
      // 1.000000001 will be truncated to 1 because fortran will think it's a float.
      Printv(value, "_", suffix, NULL);
      Delete(suffix);
    }
    Printv(f_fdecl, " ", bindc_typestr, ", parameter, public :: ", symname, " = ", value, "\n", NULL);
  } else {
    /*! Add to public fortran code:
     *
     *   IMTYPE, protected, bind(C, name="swig_SYMNAME") :: SYMNAME
     *
     * Add to wrapper code:
     *
     *   {const_CTYPE = SwigType_add_qualifier(CTYPE, "const")}
     *   {SwigType_str(const_CTYPE, swig_SYMNAME) = VALUE;}
     */
    Swig_save("constantWrapper", n, "wrap:name", "lname", NULL);

    // SYMNAME -> swig_SYMNAME
    String *wname = Swig_name_wrapper(symname);
    Setattr(n, "wrap:name", wname);

    // Set the value to replace $1 with in the 'out' typemap
    Setattr(n, "lname", value);

    // Get conversion to C type from native c++ type, *AFTER* changing
    // lname and wrap:name
    String *cwrap_code = attach_typemap("out", n, WARN_TYPEMAP_OUT_UNDEF);
    if (!cwrap_code)
      return SWIG_NOWRAP;

    int num_semicolons = 0;
    for (const char *c = Char(cwrap_code); *c != '\0'; ++c) {
      if (*c == ';')
        ++num_semicolons;
    }
    if (num_semicolons != 1) {
      // There's a newline in the output code, indicating it's
      // nontrivial.
      Swig_warning(WARN_LANG_NATIVE_UNIMPL, input_file, line_number,
                   "The 'out' typemap for '%s' must have only a single statement to wrap as a constant, but it has %d.\n",
                   symname, num_semicolons);

      return SWIG_NOWRAP;
    }

    // Get type of C value
    Swig_typemap_lookup("ctype", n, symname, NULL);
    SwigType *c_return_type = parse_typemap("ctype", n, WARN_FORTRAN_TYPEMAP_CTYPE_UNDEF);
    if (!c_return_type)
      return SWIG_NOWRAP;

    // Add a const to the return type
    SwigType_add_qualifier(c_return_type, "const");
    String *declstring = SwigType_str(c_return_type, wname);

    // Write SWIG code
    Replaceall(cwrap_code, "$result", declstring);
    Printv(f_wrapper, "SWIGEXPORT SWIGEXTERN ", cwrap_code, "\n\n", NULL);

    // Replace fclassname if needed
    this->replace_fclassname(c_return_type, bindc_typestr);

    // Add bound variable to interfaces
    Printv(f_fdecl, " ", bindc_typestr, ", protected, public, &\n",
           "   bind(C, name=\"", wname, "\") :: ",
           (Len(wname) > 60 ? "&\n    " : ""),
           symname, "\n",
           NULL);

    Swig_restore(n);
    Delete(declstring);
    Delete(wname);
  }

  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Handle a forward declaration of a class.
 */
int FORTRAN::classforwardDeclaration(Node *n) {
  // Get the class *definition* corresponding to this declaration, if any.
  Node *classn = Swig_symbol_clookup(Getattr(n, "name"), Getattr(n, "sym:symtab"));
  if (classn) {
    if (!Getattr(classn, "fortran:name") && Getattr(classn, "sym:name")) {
      // Rename the class *now* before any function has a chance to reference its type
      String *fsymname = this->make_unique_symname(classn);
      Setattr(classn, "fortran:name", fsymname);
      Delete(fsymname);
    }
  }

  return Language::classforwardDeclaration(n);
}

/* -------------------------------------------------------------------------
 * \brief Handle a forward declaration of a class.
 */
int FORTRAN::enumforwardDeclaration(Node *n) {
  if (String *name = Getattr(n, "name")) {
    // Get the class *definition* corresponding to this declaration, if any.
    Node *enumn = Swig_symbol_clookup(name, Getattr(n, "sym:symtab"));
    if (enumn) {
      if (!Getattr(enumn, "fortran:name") && Getattr(enumn, "sym:name")) {
        // Rename the class *now* before any function has a chance to reference its type
        String *fsymname = this->make_unique_symname(enumn);
        Setattr(enumn, "fortran:name", fsymname);
        Delete(fsymname);
      }
    }
  }

  return Language::enumforwardDeclaration(n);
}

/* -------------------------------------------------------------------------
 * HELPER FUNCTIONS
 * ------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------
 * \brief Substitute special '$fXXXXX' in typemaps.
 */
bool FORTRAN::replace_fclassname(SwigType *intype, String *tm) {
  assert(intype);
  bool substitution_performed = false;
  SwigType *resolvedtype = SwigType_typedef_resolve_all(intype);
  SwigType *basetype = SwigType_base(resolvedtype);

  if (Strstr(tm, "$fclassname")) {
    String *repl = get_fclassname(basetype, false);
    if (repl) {
      Replaceall(tm,  "$fclassname", repl);
      substitution_performed = true;
    }
  }
  if (Strstr(tm, "$fenumname")) {
    String *repl = get_fclassname(basetype, true);
    if (repl) {
      Replaceall(tm,  "$fenumname", repl);
      substitution_performed = true;
    }
  }

  Delete(resolvedtype);
  Delete(basetype);

  return substitution_performed;
}

/* ------------------------------------------------------------------------- */

String *FORTRAN::get_fclassname(SwigType *basetype, bool is_enum) {
  String *replacementname = NULL;
  Node *n = (is_enum ? this->enumLookup(basetype) : this->classLookup(basetype));

  if (n) {
    // Check first to see if there's a fortran symbolic name on the node
    replacementname = Getattr(n, "fortran:name");
    if (!replacementname) {
      // If not, use the symbolic name
      replacementname = Getattr(n, "sym:name");
    }
    if (is_enum && GetFlag(n, "enumMissing")) {
      // Missing enum with forward declaration
      replacementname = NULL;
    }
    if (is_enum && !GetFlag(n, "fortran:declared")) {
      // Enum is defined, but it might not have been instantiated yet
      replacementname = NULL;
    }
  } else {
    // Create a node so we can insert into the fortran symbol table
    n = NewHash();
    set_nodeType(n, "classforward");
    Setattr(n, "name", basetype);
  }

  if (!replacementname) {
    replacementname = Getattr(d_mangled_type, basetype);
    // No class/enum type or symname was found
    if (!replacementname) {
      // First time encountered with this particular class
      String *tempname = NewStringf("SWIGTYPE%s", SwigType_manglestr(basetype));
      replacementname = make_fname(tempname, WARN_NONE);
      Delete(tempname);
      if (add_fsymbol(replacementname, n) != SWIG_NOWRAP) {
        if (is_enum) {
          Replace(replacementname, "enum ", "", DOH_REPLACE_ANY);
          Printv(f_fdecl, "integer, parameter, public :: ", replacementname, " = C_INT\n", NULL);
        } else {
          // TODO: replace with this->classHandler(n);
          emit_fragment("SwigClassWrapper_f");
          Printv(f_fdecl,
                 " type, public :: ", replacementname, "\n", 
                 "  type(SwigClassWrapper), public :: swigdata\n",
                 " end type\n",
                 NULL);
        }
      }
      Setattr(d_mangled_type, basetype, replacementname);
    }
  }

  return replacementname;
}

/* ------------------------------------------------------------------------- */

void FORTRAN::replaceSpecialVariables(String *method, String *tm, Parm *parm) {
  (void)method;
  SwigType *type = Getattr(parm, "type");
  this->replace_fclassname(type, tm);
}

/* -------------------------------------------------------------------------
 * \brief Add lowercase symbol since fortran is case insensitive
 *
 * Return SWIG_NOWRAP if the name conflicts.
 */
int FORTRAN::add_fsymbol(String *s, Node *n, int warn) {
  assert(s);
  if (!is_valid_identifier(s)) {
    Swig_error(input_file, line_number,
               "The name '%s' is not a valid Fortran identifier. You must %%rename this %s.\n",
               s, nodeType(n));
    return SWIG_NOWRAP;
  }
  String *lower = Swig_string_lower(s);
  Node *existing = this->symbolLookup(lower, "fortran");

  if (existing) {
    if (warn != WARN_NONE) {
      String *n1 = get_symname_or_name(n);
      String *n2 = get_symname_or_name(existing);
      Swig_warning(warn, input_file, line_number,
                   "Ignoring '%s' due to Fortran name ('%s') conflict with '%s'\n",
                   n1, lower, n2);
    }
    Delete(lower);
    return SWIG_NOWRAP;
  }

  int success = this->addSymbol(lower, n, "fortran");
  assert(success);
  Delete(lower);
  return SWIG_OK;
}

/* -------------------------------------------------------------------------
 * \brief Make a unique fortran symbol name by appending numbers.
 */
String *FORTRAN::make_unique_symname(Node *n) {
  String *symname = Getattr(n, "sym:name");
  assert(symname);
  symname = make_fname(symname);

  // Since enum values are in the same namespace as everything else in the
  // module, make sure they're not duplicated with the scope
  Hash *symtab = this->symbolScopeLookup("fortran");

  // Lower-cased name for scope checking
  String *orig_lower = Swig_string_lower(symname);
  String *lower = Copy(orig_lower);

  int i = 0;
  while (Getattr(symtab, lower)) {
    ++i;
    Delete(lower);
    lower = NewStringf("%s%d", orig_lower, i);
  }
  if (i != 0) {
    // Warn that name has changed
    String *newname = NewStringf("%s%d", symname, i);
    Swig_warning(WARN_FORTRAN_NAME_CONFLICT, input_file, line_number,
                 "Renaming duplicate %s '%s' (Fortran name '%s')  to '%s'\n",
                 nodeType(n), symname, lower, newname);
    Delete(symname);
    symname = newname;
    // Replace symname
    Setattr(n, "sym:name", symname);
  }

  // Add lowercase name to symbol table
  Setattr(symtab, lower, n);
  Delete(orig_lower);
  Delete(lower);

  return symname;
}

/* -------------------------------------------------------------------------
 * Expose the code to the SWIG main function.
 * ------------------------------------------------------------------------- */

extern "C" Language *swig_fortran(void) {
  return new FORTRAN();
}

