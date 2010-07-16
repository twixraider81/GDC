#include "d-gcc-includes.h"
#include "d-lang.h"
#include "d-codegen.h"
#include <math.h>
#include <limits.h>
#include "total.h"
#include "template.h"
#include "init.h"
#include "symbol.h"
#include "dt.h"

#if D_GCC_VER < 40
/* struct rtx was modified for c++; this macro from rtl.h needs to
   be modified accordingly. */
#undef XEXP
#define XEXP(RTX, N)	(RTL_CHECK2 (RTX, N, 'e', 'u').rt_rtx)
#endif

static tree dt2tree(dt_t * dt);

ModuleInfo * ObjectFile::moduleInfo;
Array ObjectFile::modules;
unsigned  ObjectFile::moduleSearchIndex;
Array ObjectFile::deferredThunks;
Array ObjectFile::staticCtorList;
Array ObjectFile::staticDtorList;

typedef struct {
    tree decl;
    tree target;
    target_ptrdiff_t offset;
} DeferredThunk;

ObjectFile::ObjectFile()
{
}

void
ObjectFile::beginModule(Module * m)
{
    moduleInfo = new ModuleInfo;
    g.mod = m;
}

void
ObjectFile::endModule()
{
    for (unsigned i = 0; i < deferredThunks.dim; i++) {
	DeferredThunk * t = (DeferredThunk *) deferredThunks.data[i];
	outputThunk(t->decl, t->target, t->offset);
    }
    deferredThunks.setDim(0);
    moduleInfo = NULL;
    g.mod = NULL;
}

bool
ObjectFile::hasModule(Module *m)
{
    if (!m || ! modules.dim)
	return false;

    if (modules.data[moduleSearchIndex] == m)
	return true;
    for (unsigned i = 0; i < modules.dim; i++) {
	if ( (Module*) modules.data[i] == m ) {
	    moduleSearchIndex = i;
	    return true;
	}
    }
    return false;
}

void
ObjectFile::finish()
{
#if D_GCC_VER < 43
#define D_FFN_I 'I'
#define D_FFN_D 'D'
#else
#define D_FFN_I "I"
#define D_FFN_D "D"
#endif
    /* If the target does not directly support static constructors,
       staticCtorList contains a list of all static constructors defined
       so far.  This routine will create a function to call all of those
       and is picked up by collect2. */
    if (staticCtorList.dim) {
	doFunctionToCallFunctions(IDENTIFIER_POINTER(get_file_function_name(D_FFN_I)),
	    & staticCtorList, true);
    }
    if (staticDtorList.dim) {
	doFunctionToCallFunctions(IDENTIFIER_POINTER(get_file_function_name(D_FFN_D)),
	    & staticDtorList, true);
    }
}

void
ObjectFile::doLineNote(const Loc & loc)
{
    if (loc.filename) {
	setLoc(loc);
#if D_GCC_VER < 40
	location_t gcc_loc = { loc.filename, loc.linnum };
	emit_line_note (gcc_loc);
#endif
    } else {
	// do nothing
    }
}

#ifdef USE_MAPPED_LOCATION
static location_t
cvtLocToloc_t(const Loc loc)
{
    //assert(sizeof(StringValue.intvalue) == sizeof(location_t));
    static StringTable lmtab;
    StringValue * sv = lmtab.update(loc.filename, strlen(loc.filename));
    const struct line_map * lm = 0;
    unsigned new_line_count = 0;
    
    if (! sv->intvalue)
    {
	new_line_count = 5000;
    }
    else
    {
	lm = linemap_lookup(line_table, sv->intvalue);
	unsigned last_line = LAST_SOURCE_LINE(lm);
	if (loc.linnum > last_line)
	    new_line_count = last_line * 5;
    }
    if (new_line_count)
    {
	lm = linemap_add(line_table, LC_ENTER, 0/*...*/, loc.filename, 0);
	sv->intvalue = linemap_line_start(line_table, new_line_count, 0);
	linemap_add(line_table, LC_LEAVE, 0, NULL, 0);
	lm = linemap_lookup(line_table, sv->intvalue);
    }
    
    // cheat...
    return
    	lm->start_location + ((loc.linnum - lm->to_line) << lm->column_bits);
}
#endif

void
ObjectFile::setLoc(const Loc & loc)
{
    if (loc.filename) {
#ifdef USE_MAPPED_LOCATION
	input_location = cvtLocToloc_t(loc);
#else
	location_t gcc_loc = { loc.filename, loc.linnum };
	input_location = gcc_loc;
#endif
    } else {
	// do nothing
    }
}

void
ObjectFile::setDeclLoc(tree t, const Loc & loc)
{
    // DWARF2 will often crash if the DECL_SOURCE_FILE is not set.  It's
    // easier the error here.
    assert(loc.filename);
#ifdef USE_MAPPED_LOCATION
    DECL_SOURCE_LOCATION (t) = cvtLocToloc_t(loc);
#else
    DECL_SOURCE_FILE (t) = loc.filename;
    DECL_SOURCE_LINE (t) = loc.linnum;
#endif
}

void
ObjectFile::setDeclLoc(tree t, Dsymbol * decl)
{
    Dsymbol * dsym = decl;
    while (dsym) {
	if (dsym->loc.filename) {
	    setDeclLoc(t, dsym->loc);
	    return;
	}
	dsym = dsym->toParent();
    }

    // fallback; backend sometimes crashes if not set
    
    Loc l;

    Module * m = decl->getModule();
    if (m && m->srcfile && m->srcfile->name) {
	l.filename = m->srcfile->name->str;
    } else {
	l.filename = "<no_file>"; // Emptry string can mess up debug info
    }    
    l.linnum = 1;
    setDeclLoc(t, l); 
}

#if D_GCC_VER >= 40
void
ObjectFile::setCfunEndLoc(const Loc & loc)
{
    tree fn_decl = cfun->decl;
# ifdef USE_MAPPED_LOCATION
    if (loc.filename)
	cfun->function_end_locus = cvtLocToloc_t(loc);
    else
	cfun->function_end_locus = DECL_SOURCE_LOCATION(fn_decl);
# else
    if (loc.filename) {
	cfun->function_end_locus.file = loc.filename;
	cfun->function_end_locus.line = loc.linnum;
    } else {
	cfun->function_end_locus.file = DECL_SOURCE_FILE (fn_decl);
	cfun->function_end_locus.line = DECL_SOURCE_LINE (fn_decl);
    }
# endif    
}
#endif

void
ObjectFile::giveDeclUniqueName(tree decl, const char * prefix)
{
    /* It would be nice to be able to use TRANSLATION_UNIT_DECL
       so lhd_set_decl_assembler_name would do this automatically.
       Unforntuately, the non-NULL decl context confuses dwarf2out.

       Maybe this is fixed in later versions of GCC.
    */

    const char *name;
    if (prefix)
	name = prefix;
    else if (DECL_NAME(decl))
	name = IDENTIFIER_POINTER(DECL_NAME(decl));
    else
	name = "___s";
    char *label = d_asm_format_private_name(name, DECL_UID(decl));
    SET_DECL_ASSEMBLER_NAME(decl, get_identifier(label));
    free(label);
}

#if V2
static bool
is_function_nested_in_function(Dsymbol * dsym)
{
    FuncDeclaration * fd = dsym->isFuncDeclaration();
    AggregateDeclaration * ad;
    ClassDeclaration * cd;

    if (! fd)
	return false;
    else if (fd->isNested())
	return true;
    else if ( (ad = fd->isThis()) && (cd = ad->isClassDeclaration()) )
    {
	while (cd && cd->isNested())
	{
	    dsym = cd->toParent2();
	    if (dsym->isFuncDeclaration())
		return true;
	    else
		cd = dsym->isClassDeclaration();
	}
    }
    
    return false;
}   
#endif

void
ObjectFile::makeDeclOneOnly(tree decl_tree, Dsymbol * dsym)
{
    /* First method: Use one-only/coalesced attribute.
       
       If user has specified -femit-templates=private, honor that
       even if the target supports one-only. */
    if ( ! D_DECL_IS_TEMPLATE( decl_tree ) || gen.emitTemplates != TEprivate ) {
	/* Weak definitions have to be public.  Nested functions may or
	   may not be emitted as public even if TREE_PUBLIC is set.
	   There is no way to tell if the back end implements
	   make_decl_one_only with DECL_WEAK, so this check is
	   done first.	*/
	if ( (TREE_CODE(decl_tree) == FUNCTION_DECL &&
		decl_function_context(decl_tree) != NULL_TREE &&
		! DECL_NO_STATIC_CHAIN(decl_tree))
#if V2
	    || ( dsym && is_function_nested_in_function(dsym) )
#endif
	     ) {
	    
	    return;
	}
	    
#ifdef MAKE_DECL_COALESCED
	// %% TODO: check if available like SUPPORTS_ONE_ONLY
	// for Apple gcc...
	MAKE_DECL_COALESCED( decl_tree );
	return;
#endif
	/* The following makes assumptions about the behavior
	   of make_decl_one_only */
	if (SUPPORTS_ONE_ONLY) { // Must check, otherwise backend will abort
	    make_decl_one_only( decl_tree );
	    return;
	} else if (SUPPORTS_WEAK) {
	    tree orig_init = DECL_INITIAL( decl_tree );
	    DECL_INITIAL( decl_tree ) = integer_zero_node;
	    make_decl_one_only( decl_tree );
	    DECL_INITIAL( decl_tree ) = orig_init;
	    return;
	}
    }

    /* Second method: Make a private copy. 

       For RTTI, we can always make a private copy.  For templates, only do
       this if the user specified -femit-templates=private. */
    if ( ! D_DECL_IS_TEMPLATE( decl_tree ) || gen.emitTemplates == TEprivate ) {
	TREE_PRIVATE(decl_tree) = 1;
	TREE_PUBLIC(decl_tree) = 0;
    } else {
	static bool warned = false;
	if (! warned) {
	    warned = true;
	    ::warning(0, "system does not support one-only linkage");
	}
    }
}

void
ObjectFile::setupSymbolStorage(Dsymbol * dsym, tree decl_tree, bool force_static_public)
{
    Declaration * real_decl = dsym->isDeclaration();
    FuncDeclaration * func_decl = real_decl ? real_decl->isFuncDeclaration() : 0;
    
    if (force_static_public ||
	( TREE_CODE( decl_tree ) == VAR_DECL && (real_decl && real_decl->isDataseg()) ) ||
	( TREE_CODE( decl_tree ) == FUNCTION_DECL ) ) {

	bool is_template = false;
	Dsymbol * sym = dsym->toParent();
	Module * ti_obj_file_mod;

	while (sym) {
	    TemplateInstance * ti = sym->isTemplateInstance();
	    if (ti) {
		ti_obj_file_mod = ti->objFileModule;
		is_template = true;
		break;
	    }
	    sym = sym->toParent();
	}

	bool is_static;

	if (is_template) {
	    D_DECL_ONE_ONLY( decl_tree ) = 1;
	    D_DECL_IS_TEMPLATE( decl_tree ) = 1;
	    is_static = hasModule(ti_obj_file_mod) && gen.emitTemplates != TEnone;
	} else {
	    is_static = hasModule(dsym->getModule());
	}
	/*
	if (func_decl)
	    fprintf(stderr, "%s: is_template = %d is_static = %d te = %d m = %s cur = %s\n",
		func_decl->toPrettyChars(), is_template, is_static, emitTemplates,
		dsym->getModule() ? dsym->getModule()->toChars() : "none",
		getCurrentModule()->toChars());
	*/
	if (TREE_CODE( decl_tree ) == VAR_DECL &&
	    (real_decl && (real_decl->storage_class & STCextern)) )
	    is_static = false;
	
	if ( is_static ) {
	    DECL_EXTERNAL( decl_tree ) = 0;
	    TREE_STATIC( decl_tree ) = 1; // %% don't set until there is a body?
	    if (real_decl && (real_decl->storage_class & STCcomdat))
		D_DECL_ONE_ONLY( decl_tree ) = 1;
	} else {
	    DECL_EXTERNAL( decl_tree ) = 1;
	    TREE_STATIC( decl_tree ) = 0;
	}

	if ( ( real_decl ) ) {
	    switch (real_decl->prot()) {
	    case PROTexport:
	    case PROTpublic:
	    case PROTpackage:
	    case PROTprotected:
		// %% set for specials like init,vtbl ? -- otherwise,special case
		// for reverse the default
		TREE_PUBLIC( decl_tree ) = 1;
		break;
	    default:
		if ( (func_decl && (func_decl->isMain() || func_decl->isWinMain() ||
			  func_decl->isDllMain())) ||
		    (real_decl->isMember() && ! real_decl->isThis())) { // %% check this -- static members/
		    // DMD seems to ignore private in this case...
		    TREE_PUBLIC( decl_tree ) = 1;
		} else {
		    // From DMD:
		    /* private statics should still get a global symbol, in case
		     * another module inlines a function that references it.
		     */

		    // TREE_PUBLIC( decl_tree ) = 0;
		    TREE_PUBLIC( decl_tree ) = 1;
		}
	    }
	}

	if (D_DECL_ONE_ONLY( decl_tree ))
	    makeDeclOneOnly(decl_tree, dsym);

	if ( func_decl && func_decl->isNested() )
	    TREE_PUBLIC( decl_tree ) = 0;
    } else {
	TREE_STATIC( decl_tree ) = 0;
	DECL_EXTERNAL( decl_tree ) = 0;
	TREE_PUBLIC( decl_tree ) = 0;
    }

    if (real_decl && real_decl->attributes)
	decl_attributes(& decl_tree, gen.attributes(real_decl->attributes), 0);
}

void
ObjectFile::setupStaticStorage(Dsymbol * dsym, tree decl_tree)
{
    TREE_PUBLIC( decl_tree ) = 1; // Do this by default, but allow private templates to override
    setupSymbolStorage(dsym, decl_tree, true);
}

void
ObjectFile::outputStaticSymbol(tree t) {
    d_add_global_function(t);

    // %%TODO: flags

    /* D allows zero-length declarations.  Such a declaration ends up with
       DECL_SIZE(t) == NULL_TREE which is what the backend function
       assembler_variable checks.  This could change in later versions...
       
       Maybe all of these variables should be aliased to one symbol... */
    
    // %%TODO: could move this to lang_hooks.decls.prepare_assemble_variable to
    // make this check less precarious. -- or finish_incomplete_decl (from
    // wrapup_global_declarations
    if ( DECL_SIZE(t) == 0 ) {
	DECL_SIZE(t) = bitsize_int(0);
	DECL_SIZE_UNIT(t) = size_int(0);
    } // Otherwise, if DECL_SIZE == 0, just let it fail...
    
    rodc(t, 1);
}

void
ObjectFile::outputFunction(FuncDeclaration * f)
{
    Symbol * s = f->toSymbol();
    tree t = s->Stree;
    
    if (TREE_CODE(t) == FUNCTION_DECL)
    {
	if (DECL_STATIC_CONSTRUCTOR( t ))
	{
	    // %% check for nested function -- error
	    // otherwise, shouldn't be in a function, so safe to do asm_out

	    if (targetm.have_ctors_dtors) {
#if D_GCC_VER < 40
		(* targetm.asm_out.constructor) (XEXP (DECL_RTL (t), 0),
		    DEFAULT_INIT_PRIORITY);
#endif
		// #else, handled in d_expand_function
	    } else {
		// %% assert FuncDeclaration
		staticCtorList.push(f);
		// %%TODO: fallback if ! targetm.have_ctors_dtors
	    }
	}
	if (DECL_STATIC_DESTRUCTOR( t )) {
	    if (targetm.have_ctors_dtors) {
#if D_GCC_VER < 40
		(* targetm.asm_out.destructor) (XEXP (DECL_RTL (t), 0),
		    DEFAULT_INIT_PRIORITY);
#endif
		// #else, handled in d_expand_function
	    } else {
		// %% assert FuncDeclaration
		staticDtorList.push(f);
		// %%TODO: fallback if ! targetm.have_ctors_dtors
	    }
	}
    }

#if D_GCC_VER < 40
    rest_of_compilation(t);
#else
    if (! gen.functionNeedsChain(f))
	cgraph_finalize_function(t,
	    decl_function_context(t) != NULL);
#endif
}

bool
ObjectFile::shouldEmit(Dsymbol * d_sym)
{
    if (gen.emitTemplates != TEnone)
	return true;

    Dsymbol * sym = d_sym->toParent();
    while (sym) {
	if (sym->isTemplateInstance()) {
	    return false;
	    break;
	}
	sym = sym->toParent();
    }
    return true;
}

bool
ObjectFile::shouldEmit(Symbol * sym)
{
    return (gen.emitTemplates != TEnone) || ! D_DECL_IS_TEMPLATE(sym->Stree);
}

void
ObjectFile::addAggMethods(tree rec_type, AggregateDeclaration * agg)
{
    if (write_symbols != NO_DEBUG) {
	ListMaker methods;
	for (unsigned i = 0; i < agg->methods.dim; i++) {
	    FuncDeclaration * fd = (FuncDeclaration *) agg->methods.data[i];
	    
	    methods.chain(fd->toSymbol()->Stree);
	}
	TYPE_METHODS( rec_type ) = methods.head;
    }
}

void
ObjectFile::initTypeDecl(tree t, Dsymbol * d_sym)
{
    assert( ! POINTER_TYPE_P( t ) );
    if (! TYPE_STUB_DECL(t))
    {
	const char * name = d_sym->ident ? d_sym->ident->string : "fix";
	tree decl = build_decl(TYPE_DECL, get_identifier(name), t);
	DECL_CONTEXT( decl ) = gen.declContext(d_sym);
	setDeclLoc(decl, d_sym);
	initTypeDecl(t, decl);
    }
}


void
ObjectFile::declareType(tree t, Type * d_type)
{
    // Note: It is not safe to call d_type->toCtype().
    Loc l;
    tree decl = build_decl(TYPE_DECL, get_identifier(d_type->toChars()), t);
    l.filename = "<internal>";
    l.linnum = 1;
    setDeclLoc(decl, l);

    declareType(t, decl);
}

void
ObjectFile::declareType(tree t, Dsymbol * d_sym)
{
    initTypeDecl(t, d_sym);
    declareType(t, TYPE_NAME(t));
}


void
ObjectFile::initTypeDecl(tree t, tree decl)
{
    if ( ! TYPE_STUB_DECL(t))
    {
	assert( ! POINTER_TYPE_P( t ) );

	TYPE_CONTEXT( t ) = DECL_CONTEXT( decl );
	TYPE_NAME( t ) = decl;
	switch (TREE_CODE(t))
	{
	case ENUMERAL_TYPE:
	case RECORD_TYPE:
	case UNION_TYPE:
	    /* Not sure if there is a need for separate TYPE_DECLs in
	       TYPE_NAME and TYPE_STUB_DECL. */
	    TYPE_STUB_DECL( t ) = build_decl(TYPE_DECL, DECL_NAME(decl), t);
#if D_GCC_VER >= 43
	    DECL_ARTIFICIAL( TYPE_STUB_DECL( t ) ) = 1; // dunno...
	    // code now assumes...
	    DECL_SOURCE_LOCATION(TYPE_STUB_DECL(t)) = DECL_SOURCE_LOCATION(decl);
#else
	    // g++ does this and the debugging code assumes it:
	    DECL_ARTIFICIAL( TYPE_STUB_DECL( t ) ) = 1;
#endif	    
	    break;
	default:
	    // nothing
	    break;
	}
    }
}

void
ObjectFile::declareType(tree t, tree decl)
{
    bool top_level = /*DECL_CONTEXT(decl) == fileContext || */
	! DECL_CONTEXT(decl);
	
    // okay to do this?
    rodc(decl, top_level);
}

tree
ObjectFile::stripVarDecl(tree value) {
    if ( TREE_CODE( value ) != VAR_DECL ) {
	return value;
    } else if ( DECL_INITIAL(value) ) {
	return DECL_INITIAL(value);
    } else {
	Type * d_type = gen.getDType( TREE_TYPE( value ));
	if (d_type) {
	    d_type = d_type->toBasetype();
	    switch (d_type->ty) {
	    case Tstruct:
		{
		    //tree t = gen.aggregateInitializer(((TypeStruct *) d_type)->sym, NULL, NULL, NULL_TREE);
		    // %% maker sure this is doing the right thing...
		    // %% better do get rid of it..

		    // need to VIEW_CONVERT?
		    dt_t * dt = NULL;
		    d_type->toDt(& dt);
		    tree t = dt2tree( dt );
		    TREE_CONSTANT(t) = 1;
		    return t;
		}
		break;
	    default:
		// error below
		break;
	    }
	}
	abort();
    }
}

void
ObjectFile::doThunk(tree thunk_decl, tree target_decl, target_ptrdiff_t offset)
{
    if (current_function_decl) {
	DeferredThunk * t = new DeferredThunk;
	t->decl = thunk_decl;
	t->target = target_decl;
	t->offset = offset;
	deferredThunks.push(t);
    } else {
	outputThunk(thunk_decl, target_decl, offset);
    }
}

/* Thunk code is based on g++ */

#ifdef ASM_OUTPUT_DEF
static int thunk_labelno;

/* Create a static alias to function.  */

static tree
make_alias_for_thunk (tree function)
{
  tree alias;
  char buf[256];

#if defined (TARGET_IS_PE_COFF)
  /* make_alias_for_thunk does not seem to be needed for TARGET_IS_PE_COFF
     at all, and apparently causes problems... */
  //if (DECL_ONE_ONLY (function))
    return function;
#endif
  // For gdc: Thunks may reference extern functions which cannot be aliased.
  if (DECL_EXTERNAL(function))
      return function;

  ASM_GENERATE_INTERNAL_LABEL (buf, "LTHUNK", thunk_labelno);
  thunk_labelno++;
  alias = build_decl (FUNCTION_DECL, get_identifier (buf),
		      TREE_TYPE (function));
  DECL_CONTEXT (alias) = NULL;
  TREE_READONLY (alias) = TREE_READONLY (function);
  TREE_THIS_VOLATILE (alias) = TREE_THIS_VOLATILE (function);
  TREE_PUBLIC (alias) = 0;
  DECL_EXTERNAL (alias) = 0;
  DECL_ARTIFICIAL (alias) = 1;
  DECL_NO_STATIC_CHAIN (alias) = 1;
  DECL_INLINE (alias) = 0;
  DECL_DECLARED_INLINE_P (alias) = 0;
  //DECL_INITIAL (alias) = error_mark_node;
  TREE_ADDRESSABLE (alias) = 1;
  TREE_USED (alias) = 1;
  SET_DECL_ASSEMBLER_NAME (alias, DECL_NAME (alias));
  TREE_SYMBOL_REFERENCED (DECL_ASSEMBLER_NAME (alias)) = 1;
  if (!flag_syntax_only)
    assemble_alias (alias, DECL_ASSEMBLER_NAME (function));
  return alias;
}
#endif

void
ObjectFile::outputThunk(tree thunk_decl, tree target_decl, target_ptrdiff_t offset)
{
    target_ptrdiff_t delta = -offset;
    tree alias;

#ifdef ASM_OUTPUT_DEF
    alias = make_alias_for_thunk(target_decl);
#else
    alias = target_decl;
#endif

    TREE_ADDRESSABLE(target_decl) = 1;
    TREE_USED(target_decl) = 1;
    DECL_VISIBILITY (thunk_decl) = DECL_VISIBILITY (target_decl);
#if ASM_OUTPUT_DEF \
   && !defined (TARGET_IS_PE_COFF)
  if (targetm.have_named_sections)
    {
      resolve_unique_section (target_decl, 0, flag_function_sections);

      if (DECL_SECTION_NAME (target_decl) != NULL && DECL_ONE_ONLY (target_decl))
	{
	  resolve_unique_section (thunk_decl, 0, flag_function_sections);

	  /* Output the thunk into the same section as function.  */
	  DECL_SECTION_NAME (thunk_decl) = DECL_SECTION_NAME (target_decl);
	}
    }
#endif
  // cp/method.c:
  /* The back-end expects DECL_INITIAL to contain a BLOCK, so we
     create one.  */
  // ... actually doesn't seem to be the case for output_mi_thunk
  DECL_INITIAL (thunk_decl) = make_node (BLOCK);
  BLOCK_VARS (DECL_INITIAL (thunk_decl)) = DECL_ARGUMENTS (thunk_decl);

    if (targetm.asm_out.can_output_mi_thunk(thunk_decl, delta, 0, alias)) {
	const char *fnname;

	current_function_decl = thunk_decl;
	DECL_RESULT( thunk_decl ) = build_decl(RESULT_DECL, 0, integer_type_node);
	
	fnname = XSTR(XEXP(DECL_RTL(thunk_decl), 0), 0);
	gen.initFunctionStart(thunk_decl, 0);
	current_function_is_thunk = 1;
	assemble_start_function (thunk_decl, fnname);
	targetm.asm_out.output_mi_thunk (asm_out_file, thunk_decl,
	    delta, 0, alias);
	assemble_end_function(thunk_decl, fnname);
	current_function_decl = 0;
	set_cfun(0);
#if D_GCC_VER < 40
	/* Because init_function_start increments this, we must
	   decrement it.  */
	immediate_size_expand--;
#endif
	TREE_ASM_WRITTEN (thunk_decl) = 1;
    } else {
	sorry("backend for this target machine does not support thunks");
    }
}

FuncDeclaration *
ObjectFile::doSimpleFunction(const char * name, tree expr, bool static_ctor, bool public_fn)
{
    Module * mod = g.mod;
    if (! mod)
	mod = d_gcc_get_output_module();

    if (name[0] == '*') {
	Symbol * s = mod->toSymbolX(name + 1, 0, 0, "FZv");
	name = s->Sident;
    }
    
    TypeFunction * func_type = new TypeFunction(0, Type::tvoid, 0, LINKc);
    FuncDeclaration * func = new FuncDeclaration(mod->loc, mod->loc, // %% locs may be wrong
	Lexer::idPool(name), STCstatic, func_type); // name is only to prevent crashes
    func->loc = Loc(mod, 1); // to prevent debug info crash // maybe not needed if DECL_ARTIFICIAL?
    func->linkage = func_type->linkage;
    func->parent = mod;
    func->protection = public_fn ? PROTpublic : PROTprivate;

    tree func_decl = func->toSymbol()->Stree;
    if (static_ctor)
	DECL_STATIC_CONSTRUCTOR( func_decl ) = 1; // apparently, the back end doesn't do anything with this

    // D static ctors, dtors, unittests, and the ModuleInfo chain function
    // are always private (see ObjectFile::setupSymbolStorage, default case)
    TREE_PUBLIC( func_decl ) = public_fn;
    
    // %% maybe remove the identifier

    func->fbody = new ExpStatement(mod->loc,
	new WrappedExp(mod->loc, TOKcomma, expr, Type::tvoid));

    func->toObjFile(false);

    return func;
}

/* force: If true, create a new function even there is only one function in the
          list.
*/
FuncDeclaration *
ObjectFile::doFunctionToCallFunctions(const char * name, Array * functions, bool force_and_public)
{
    Module * mod = g.mod;

    // If there is only one function, just return that
    if (functions->dim == 1 && ! force_and_public) {
	return (FuncDeclaration *) functions->data[0];
    } else if (functions->dim >= 1) {
	// %% shouldn't front end build these?
	tree exp = NULL_TREE;
	tree call_exp = NULL_TREE;
	for (unsigned i = 0; i < functions->dim; i++) {
	    FuncDeclaration * fn_decl = (FuncDeclaration *) functions->data[i];
	    call_exp = gen.buildCall(void_type_node, gen.addressOf(fn_decl), NULL_TREE);
	    if (! exp)
		exp = call_exp;
	    else {
		exp = build2(COMPOUND_EXPR, void_type_node, exp, call_exp);
	    }
	}

	return doSimpleFunction(name, exp, false, force_and_public);
    } else {
	return NULL;
    }
}

dt_t**
dtnwords(dt_t** pdt, size_t word_count, void * pwords, unsigned word_size)
{
    return dtnbytes(pdt, word_count * word_size,
	gen.hostToTargetString((char*) pwords, word_count, word_size));
}

dt_t**
dtawords(dt_t** pdt, size_t word_count, void * pwords, unsigned word_size)
{
    return dtabytes(pdt, TYnptr, 0, word_count * word_size,
	gen.hostToTargetString((char*) pwords, word_count, word_size));
}

/* Add a 32-bit value to a dt.  If pad_to_word is true, adds any
   necessary padding so that the next value is aligned to PTRSIZE. */
dt_t**
dti32(dt_t** pdt, unsigned val, int pad_to_word)
{
    dt_t** result = dttree(pdt, gen.integerConstant(val, Type::tuns32));
    if (! pad_to_word || PTRSIZE == 4)
	return result;
    else if (PTRSIZE == 8)
	return dttree(result, gen.integerConstant(0, Type::tuns32));
    else
	gcc_unreachable();
}

dt_t**
dtcontainer(dt_t** pdt, Type * type, dt_t* values)
{
    dt_t * d = new dt_t;
    d->dt = DT_container;
    d->DTnext = 0;
    d->DTtype = type;
    d->DTvalues = values;
    return dtcat(pdt, d);
}



target_size_t
dt_size(dt_t * dt)
{
    target_size_t size = 0;

    while (dt) {
	switch (dt->dt) {
	    case DT_azeros:
	    case DT_common:
	    case DT_nbytes:	
		size += dt->DTint;
		break;
	    case DT_abytes:
	    case DT_xoff:
		size += PTRSIZE;
		break;
	    case DT_word:
		// This may cause problems for 64-bit since dtdword is used for
		// (uint? / size_t?) and pointers... Nearly all cases are for size_t
		// and pointer, though.
		size += PTRSIZE;
		break;
	    case DT_tree:
		{
		    tree t_size = TYPE_SIZE_UNIT( TREE_TYPE( dt->DTtree ));
		    size += gen.getTargetSizeConst(t_size);
		}
		break;
	    case DT_container:
		size += dt_size(dt->DTvalues);
		break;
	    default:
		assert(0);
	}
	dt = dt->DTnext;
    }

    return size;
}

static tree
check_static_sym(Symbol * sym)
{
    if (! sym->Stree) {
	//assert( sym->Sdt );// Unfortunately cannot check for this; it might be an empty dt_t list...

	tree t_ini = dt2tree( sym->Sdt ); // %% recursion problems?
	tree t_var = build_decl( VAR_DECL, NULL_TREE, TREE_TYPE( t_ini ));
	g.ofile->giveDeclUniqueName(t_var);
	DECL_INITIAL( t_var ) = t_ini;
	TREE_STATIC( t_var ) = 1;
	if (sym->Sseg == CDATA) {
	    TREE_CONSTANT( t_var ) = TREE_CONSTANT( t_ini ) = 1;
	    TREE_READONLY( t_var ) = TREE_READONLY( t_ini ) = 1;
	}
	// %% need to check SCcomdat?
	
	TREE_PRIVATE( t_var ) = 1;
	DECL_IGNORED_P( t_var ) = 1;
	DECL_ARTIFICIAL( t_var ) = 1;
	sym->Stree = t_var;
    }
    return sym->Stree;
}

static tree
dt2tree_list_of_elems(dt_t * dt);

static tree
dt2node(dt_t * dt)
{
    switch (dt->dt) {
    case DT_azeros:
    case DT_common:
	{
	    tree a = make_node(CONSTRUCTOR);
	    TREE_TYPE(a) = gen.arrayType(Type::tuns8, dt->DTint);
	    TREE_READONLY(a) = 1;
	    TREE_CONSTANT(a) = 1;
	    return a;
	}
    case DT_nbytes:
	{
	    tree s = build_string(dt->DTint, (char *) dt->DTpointer);
	    TREE_TYPE( s ) = gen.arrayType(Type::tuns8, dt->DTint);	    
	    return s;
	}
    case DT_abytes:
	{
	    tree s = build_string(dt->DTint, (char *) dt->DTpointer);
	    TREE_TYPE( s ) = gen.arrayType(Type::tuns8, dt->DTint);
	    TREE_STATIC( s ) = 1;
	    return gen.addressOf( s );
	}
    case DT_word:
	// %% make sure this is the target word type
	return gen.integerConstant(dt->DTint, Type::tsize_t);
    case DT_xoff:
	return gen.pointerOffset(
	    gen.addressOf(check_static_sym(dt->DTsym)),
	    gen.integerConstant(dt->DTint, Type::tsize_t) );
    case DT_tree:
	return dt->DTtree;
    case DT_container:
	{
	    /* It is necessary to give static array data its original
	       type.  Otherwise, the SRA pass will not find the array
	       elements.

	       SRA accesses struct elements by field offset, so the ad
	       hoc type from dt2tree is fine.  It must still be a
	       CONSTRUCTOR, or the CCP pass may use it incorrectly.
	    */
	    Type *tb = NULL;
	    if (dt->DTtype)
		tb = dt->DTtype->toBasetype();
	    if (tb && tb->ty == Tsarray)
	    {
		TypeSArray * tsa = (TypeSArray *) tb;
		CtorEltMaker ctor_elts;
		dt_t * dte = dt->DTvalues;
		target_size_t i = 0;
		ctor_elts.reserve(tsa->dim->toInteger());
		while (dte)
		{
		    ctor_elts.cons(gen.integerConstant(i++, size_type_node),
			dt2node(dte));
		    dte = dte->DTnext;
		}
		tree ctor = make_node(CONSTRUCTOR);
		TREE_TYPE(ctor) = dt->DTtype->toCtype();

		// DT data should always be constant.  If the decl is not TREE_CONSTANT, fine.
		TREE_CONSTANT(ctor) = 1;

		TREE_READONLY(ctor) = 1;
		TREE_STATIC(ctor) = 1;
		CONSTRUCTOR_ELTS(ctor) = ctor_elts.head;
		return ctor;
	    }
	    else if (tb && tb->ty == Tstruct)
		return dt2tree_list_of_elems(dt->DTvalues);
	    else
		return dt2tree(dt->DTvalues);
	}
    default:
	abort();
    }
    return NULL;
}

static tree
dt2tree(dt_t * dt)
{
    if (dt && /*dt->dt == DT_container || */dt->DTnext == NULL)
	return dt2node(dt);
    else
	return dt2tree_list_of_elems(dt);
}

static tree
dt2tree_list_of_elems(dt_t * dt)
{
    // Generate type on the fly
    CtorEltMaker elts;
    ListMaker fields;
    tree offset = size_zero_node;

    tree aggtype = make_node(RECORD_TYPE);

    while (dt) {
	tree value = dt2node(dt);
	tree field = build_decl(FIELD_DECL, NULL_TREE, TREE_TYPE(value));
	DECL_CONTEXT(field) = aggtype;
	DECL_FIELD_OFFSET(field) = offset;
	DECL_FIELD_BIT_OFFSET(field) = bitsize_zero_node;
	SET_DECL_OFFSET_ALIGN(field, TYPE_ALIGN( TREE_TYPE( field )));
	DECL_ARTIFICIAL(field) = 1;
	DECL_IGNORED_P(field) = 1;
	layout_decl(field, 0);

	fields.chain(field);
	elts.cons(field, value);
	offset = size_binop(PLUS_EXPR, offset, TYPE_SIZE_UNIT( TREE_TYPE( value )));

	dt = dt->DTnext;
    }

    TYPE_FIELDS(aggtype) = fields.head; // or finish_laout
    TYPE_SIZE(aggtype) = convert( bitsizetype,
	size_binop( MULT_EXPR, offset, size_int( BITS_PER_UNIT )));
    TYPE_SIZE_UNIT(aggtype) = offset;
    // okay no alignment -- decl (which has the correct type) should take care of it..
    // align=bits per word?
    compute_record_mode(aggtype);

    tree ctor = make_node(CONSTRUCTOR);
    TREE_TYPE(ctor) = aggtype;
    TREE_READONLY(ctor) = 1;
    // dt created data is always static
    TREE_STATIC(ctor) = 1;
    // should always be constant
    TREE_CONSTANT(ctor) = 1;

    // DT data should always be constant.  If the decl is not TREE_CONSTANT, fine.
    CONSTRUCTOR_ELTS(ctor) = elts.head;
    return ctor;
}

void
outdata(Symbol * sym)
{
    tree t = check_static_sym( sym );

    assert( t );

    if (sym->Sdt && DECL_INITIAL( t ) == NULL_TREE)
	DECL_INITIAL( t ) = dt2tree( sym->Sdt );

    if (! g.ofile->shouldEmit(sym))
	return;

    if ( DECL_EXTERNAL( t ) ) {
	// %% Oops, this was supposed to be static.
	// This is for typeinfo decls ... may
	// insert into module instead?
	DECL_EXTERNAL( t ) = 0;
	TREE_STATIC( t ) = 1;
    }
    
    layout_decl(t, 0);

    // from outputStaticSymbol:
    if ( DECL_SIZE(t) == NULL_TREE ) {
	DECL_SIZE(t) = bitsize_int(0);
	DECL_SIZE_UNIT(t) = size_int(0);
    } // Otherwise, if DECL_SIZE == 0, just let it fail...

    d_add_global_function(t);

    g.ofile->rodc(t, 1);
}

void
obj_includelib(const char *name)
{
    d_warning(OPT_Wunknown_pragmas, "pragma(lib) not implemented");
}

void
obj_startaddress(Symbol *s)
{
    d_warning(OPT_Wunknown_pragmas, "pragma(startaddress) not implemented");
}

void
obj_append(Dsymbol *s)
{
    // GDC does not do multi-obj, so just write it out now.
    s->toObjFile(false);
}

void
obj_moduleinfo(Symbol *sym)
{
    /*
	Generate:
	struct _modref_t {
	    _modref_t  * next;
	    ModuleInfo m;
	}
	extern (C) _modref_t * _Dmodule_ref;
	private _modref_t our_mod_ref =
	  { next: null, m: _ModuleInfo_xxx };
	void ___modinit() { // a static constructor
	   our_mod_ref.next = _Dmodule_ref;
	   _Dmodule_ref = & our_mod_ref;
	}
    */
    // struct ModuleReference in moduleinit.d
    tree mod_ref_type = gen.twoFieldType(Type::tvoid->pointerTo(), gen.getObjectType(),
	NULL, "next", "mod");
    tree f0 = TYPE_FIELDS( mod_ref_type );
    tree f1 = TREE_CHAIN( f0 );

    tree our_mod_ref = build_decl(VAR_DECL, NULL_TREE, mod_ref_type);
    dkeep(our_mod_ref);
    g.ofile->giveDeclUniqueName(our_mod_ref, "__mod_ref");
    g.ofile->setDeclLoc(our_mod_ref, g.mod);

    tree init = make_node(CONSTRUCTOR);
    CtorEltMaker ce;
    TREE_TYPE( init ) = mod_ref_type;
    ce.cons(f0, d_null_pointer);
    ce.cons(f1, gen.addressOf(sym->Stree));
    CONSTRUCTOR_ELTS( init ) = ce.head;
    TREE_STATIC( init ) = 1;
    DECL_ARTIFICIAL( our_mod_ref ) = 1;
    DECL_IGNORED_P( our_mod_ref ) = 1;
    TREE_PRIVATE( our_mod_ref ) = 1;
    TREE_STATIC( our_mod_ref ) = 1;
    DECL_INITIAL( our_mod_ref ) = init;
    g.ofile->rodc(our_mod_ref, 1);

    tree the_mod_ref = build_decl(VAR_DECL, get_identifier("_Dmodule_ref"),
	build_pointer_type( mod_ref_type ));
    dkeep(the_mod_ref);
    DECL_EXTERNAL( the_mod_ref ) = 1;
    TREE_PUBLIC( the_mod_ref ) = 1;

    tree m1 = build2(MODIFY_EXPR, void_type_node,
	gen.component(our_mod_ref, f0),
	the_mod_ref);
    tree m2 = build2(MODIFY_EXPR, void_type_node,
	the_mod_ref, gen.addressOf(our_mod_ref));
    tree exp = build2(COMPOUND_EXPR, void_type_node, m1, m2);

    g.ofile->doSimpleFunction("*__modinit", exp, true);
}

