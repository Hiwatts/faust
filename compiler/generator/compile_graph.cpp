/************************************************************************
 ************************************************************************
    FAUST compiler
    Copyright (C) 2003-2018 GRAME, Centre National de Creation Musicale
    ---------------------------------------------------------------------
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 ************************************************************************
 ************************************************************************/

/*****************************************************************************
    HISTORY
    22/01/05 : corrected bug on bool signals cached in float variables
*****************************************************************************/

#include "compile_graph.hh"
#include <math.h>
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include "compatibility.hh"
#include "compile.hh"
#include "compile_scal.hh"
#include "delayLineSimplifier.hh"
#include "floats.hh"
#include "ppsig.hh"
#include "prim2.hh"
#include "privatise.hh"
#include "recursivness.hh"
#include "scalarScheduling.hh"
#include "sigConstantPropagation.hh"
#include "sigPromotion.hh"
#include "sigToGraph.hh"
#include "signalDependencies.hh"
#include "signalGraph.hh"
#include "signalSplitter.hh"
#include "signalVisitor.hh"
#include "sigprint.hh"
#include "sigtype.hh"
#include "sigtyperules.hh"
#include "simplify.hh"
#include "splitCommonSubexpr.hh"
#include "timing.hh"
#include "transformDelayToTable.hh"
#include "transformOld2NewTables.hh"
#include "xtended.hh"

using namespace std;

static Klass* signal2klass(Klass* parent, const string& name, Tree sig)
{
    Type t = getCertifiedSigType(sig);  //, NULLENV);
    if (t->nature() == kInt) {
        ScalarCompiler C(new SigIntGenKlass(parent, name));
        C.compileSingleSignal(sig);
        return C.getClass();

    } else {
        ScalarCompiler C(new SigFloatGenKlass(parent, name));
        C.compileSingleSignal(sig);
        return C.getClass();
    }
}
static bool isInit(Tree i)
{
    Tree id, origin, exp;
    int  nat;
    bool r = isSigInstructionControlWrite(i, id, origin, &nat, exp);

    if (r) {
        Type t = getCertifiedSigType(origin);
        // cerr << "-> " << *t << " -> " << ppsig(i) << endl;
        if (t->variability() == kKonst) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if a control instruction
 *
 * @param i the instruction to test
 * @return true if it is a control instruction
 * @return false otherwise
 */
static bool isControl(Tree i)
{
    Tree id, origin, exp;
    int  nat;
    bool r = isSigInstructionControlWrite(i, id, origin, &nat, exp);
#if 0
    if (r) {
        Type t = getCertifiedSigType(origin);
        cerr << "-> " << *t << " -> " << ppsig(i) << endl;
    }
#endif
    return r;
}

/*****************************************************************************
 getFreshID
 *****************************************************************************/

map<string, int> GraphCompiler::fIDCounters;

string GraphCompiler::getFreshID(const string& prefix)
{
    if (fIDCounters.find(prefix) == fIDCounters.end()) {
        fIDCounters[prefix] = 0;
    }
    int n               = fIDCounters[prefix];
    fIDCounters[prefix] = n + 1;
    return subst("$0$1", prefix, T(n));
}

/*****************************************************************************
 prepare
 *****************************************************************************/

Tree GraphCompiler::prepare(Tree LS)
{
    startTiming("GraphCompiler::prepare");
    //  startTiming("first simplification");
    //  LS = simplify(LS);
    //  endTiming("first simplification");
    startTiming("deBruijn2Sym");
    Tree L1 = deBruijn2Sym(LS);  // convert debruijn recursion into symbolic recursion
    endTiming("deBruijn2Sym");

    startTiming("L1 typeAnnotation");
    typeAnnotation(L1, gGlobal->gLocalCausalityCheck);
    endTiming("L1 typeAnnotation");

    startTiming("Cast and Promotion");
    SignalPromotion SP;
    SP.trace(gGlobal->gDebugSwitch, "Cast");
    Tree L1b = SP.mapself(L1);
    endTiming("Cast and Promotion");

    startTiming("second simplification");
    Tree L2 = simplify(L1b);  // simplify by executing every computable operation
    endTiming("second simplification");

    startTiming("Constant propagation");
    SignalConstantPropagation SK;
    SK.trace(gGlobal->gDebugSwitch, "ConstProp2");
    Tree L2b = SK.mapself(L2);
    endTiming("Constant propagation");

    Tree L3 = privatise(L2b);  // Un-share tables with multiple writers

    conditionAnnotation(L3);
    // conditionStatistics(L3);        // count condition occurences

    // dump normal form
    if (gGlobal->gDumpNorm) {
        cout << ppsig(L3) << endl;
        throw faustexception("Dump normal form finished...\n");
    }

    recursivnessAnnotation(L3);  // Annotate L3 with recursivness information

    startTiming("typeAnnotation");
    typeAnnotation(L3, true);  // Annotate L3 with type information
    endTiming("typeAnnotation");

    sharingAnalysis(L3);  // annotate L3 with sharing count

    if (fOccMarkup != nullptr) {
        delete fOccMarkup;
    }
    fOccMarkup = new old_OccMarkup(fConditionProperty);
    fOccMarkup->mark(L3);  // annotate L3 with occurences analysis

    // sharingAnalysis(L3);  // annotate L3 with sharing count

    if (gGlobal->gDrawSignals) {
        ofstream dotfile(subst("$0-sig.dot", gGlobal->makeDrawPath()).c_str());
        // SL : 28/09/17 : deactivated for now
        // sigToGraph(L3, dotfile);
    }

    return L3;
}

Tree GraphCompiler::prepare2(Tree L0)
{
    startTiming("GraphCompiler::prepare2");

    recursivnessAnnotation(L0);  // Annotate L0 with recursivness information
    typeAnnotation(L0, true);    // Annotate L0 with type information
    sharingAnalysis(L0);         // annotate L0 with sharing count

    if (fOccMarkup != nullptr) {
        delete fOccMarkup;
    }
    fOccMarkup = new old_OccMarkup();
    fOccMarkup->mark(L0);  // annotate L0 with occurences analysis

    endTiming("GraphCompiler::prepare2");
    return L0;
}

/**
 * @brief prepare3(L): prepare a list of expressions to be ready to be translated into a set of instructions
 *
 * @param L1
 * @return Tree
 */
Tree GraphCompiler::prepare3(Tree L1)
{
    recursivnessAnnotation(L1);  // Annotate L0 with recursivness information
    typeAnnotation(L1, true);    // Annotate L0 with type information

    SignalPromotion SP;
    Tree            L1b = SP.mapself(L1);

    Tree L2 = simplify(L1b);  // simplify by executing every computable operation

    SignalConstantPropagation SK;
    Tree                      L2b = SK.mapself(L2);

    // Tree L3 = privatise(L2b);  // Un-share tables with multiple writers

    return L2b;
}

/**
 * @brief expression2Instructions() : transform a single expression into
 * a set of instructions
 * @param exp the expression to transform
 * @return set<Tree>
 */
set<Tree> GraphCompiler::expression2Instructions(Tree exp)
{
    Tree L = cons(exp, gGlobal->nil);
    Tree P = prepare3(L);
    return ExpressionsListToInstructionsSet(P);
}

/**
 * @brief collect the IDS of all the table used in a set of instructions
 *
 * @param I
 * @return set<Tree>
 */
set<Tree> GraphCompiler::collectTableIDs(const set<Tree> I)
{
    set<Tree> IDs;
    for (Tree i : I) {
        Tree id, origin, ginit, init, widx, wsig;
        int  nature, tblsize;
        if (isSigInstructionTableWrite(i, id, origin, &nature, &tblsize, ginit, widx, wsig)) {
            int    n;
            double r;
            if (isSigGen(ginit, init)) {
                if (isSigInt(init, &n) || isSigReal(init, &r)) {
                    // trivial init
                } else {
                    IDs.insert(id);
                    fTableInitExpression.set(id, init);
                    fTableInitSize.set(id, tblsize);
                    fTableInitNature.set(id, nature);
                    // cerr << "\ncollectTableIDs: " << id << "@" << *id << " with size " << tblsize << " and with init
                    // "
                    //      << ppsig(init) << endl;
                }
            }
        }
    }
    return IDs;
}

/**
 * @brief make sure each instruction is properly typed
 *
 * @param I the sinstruction set
 */
static void typeAnnotateInstructionSet(const set<Tree>& I)
{
    for (Tree i : I) {
        Type ty = getSimpleType(i);
    }
}

/**
 * @brief ExpressionsListToInstructionsSet(): transfoms a list of signals expressions
 * into a set of instructions
 *
 * @param L3 a list of expressions
 * @return set<Tree> the resulting set of instructions
 */
set<Tree> GraphCompiler::ExpressionsListToInstructionsSet(Tree L3)
{
    // Each expression represent an output. We decorate them with
    // output informations
    Tree L3d  = gGlobal->nil;
    int  onum = 0;
    for (Tree l = L3; isList(l); l = tl(l)) {
        L3d = cons(sigOutput(onum++, hd(l)), L3d);
    }
    L3d = reverse(L3d);
    recursivnessAnnotation(L3d);  // Annotate L3 with recursivness information

    startTiming("typeAnnotation");
    typeAnnotation(L3d, true);  // Annotate L3 with type information
    endTiming("typeAnnotation");

    // cerr << ">>Transformation into Instructions\n" << endl;
    startTiming("Transformation into Instructions");
    set<Tree> INSTR1 = splitSignalsToInstr(fConditionProperty, L3d);
    typeAnnotateInstructionSet(INSTR1);
    if (gGlobal->gDebugSwitch) signalGraph("phase1-beforeSimplification.dot", INSTR1);

    // // cerr << ">>delayLineSimplifier\n" << endl;
    // set<Tree> INSTR2 = delayLineSimplifier(INSTR1);
    // typeAnnotateInstructionSet(INSTR2);
    // if (gGlobal->gDebugSwitch) signalGraph("phase2-afterSimplification.dot", INSTR2);

    // cerr << ">>transformDelayToTable\n" << endl;
    set<Tree> INSTR3 = transformDelayToTable(INSTR1);
    typeAnnotateInstructionSet(INSTR3);
    if (gGlobal->gDebugSwitch) signalGraph("phase3-afterTable.dot", INSTR3);

    // cerr << ">>transformOld2NewTables\n" << endl;
    set<Tree> INSTR4 = transformOld2NewTables(INSTR3);
    typeAnnotateInstructionSet(INSTR4);
    if (gGlobal->gDebugSwitch) signalGraph("phase4-afterTableTransform.dot", INSTR4);

    // cerr << ">>splitCommonSubexpr\n" << endl;
    set<Tree> INSTR5 = splitCommonSubexpr(INSTR4);
    typeAnnotateInstructionSet(INSTR5);
    if (gGlobal->gDebugSwitch) signalGraph("phase5-afterCSE.dot", INSTR5);

#if 0
    cerr << "Start scalarscheduling" << endl;
    scalarScheduling("phase5-scalarScheduling.txt", INSTR4);

    cerr << "Start parallelScheduling" << endl;
    parallelScheduling("phase6-parallelScheduling.txt", INSTR4);

    endTiming("Transformation into Instructions");
#endif
    return INSTR5;
}

/**
 * @brief Schedule a set of instructions into init, block and
 * exec instruction sequences
 *
 * @param I the set of instructions to schedule
 * @return the sequential Scheduling
 */
Scheduling GraphCompiler::schedule(const set<Tree>& I)
{
    digraph<Tree> G;  // the signal graph
    Scheduling    S;

    // 1) build the graph and the dictionnary
    for (auto i : I) {
        G.add(dependencyGraph(i));
        S.fDic.add(i);
    }

    digraph<Tree> T;  // the subgraph of control instructions (temporary)
    digraph<Tree> K;  // the subgraph of init-time instructions
    digraph<Tree> B;  // the subgraph of block-time instructions
    digraph<Tree> E;  // the subgraph at sample-time instructions

    // 2) split in three sub-graphs: K, B, E

    splitgraph<Tree>(
        G, [&S](Tree id) { return isControl(S.fDic[id]); }, T, E);
    splitgraph<Tree>(
        T, [&S](Tree id) { return isInit(S.fDic[id]); }, K, B);

    // 3) fill the scheduling

    // a) for the init and block level graph we know they don't have cycles
    // and can be directly serialized
    for (Tree i : serialize(K)) S.fInitLevel.push_back(i);
    for (Tree i : serialize(B)) S.fBlockLevel.push_back(i);

    // b) for the sample level graph we have (probably) cycles
    digraph<digraph<Tree>> DG = graph2dag(E);
    vector<digraph<Tree>>  VG = serialize(DG);
    for (digraph<Tree> g : VG) {
        vector<Tree> v = serialize(cut(g, 1));
        for (Tree i : v) {
            S.fExecLevel.push_back(i);
        }
    }

    return S;
}

/*****************************************************************************
 compileMultiSignal
 *****************************************************************************/

string GraphCompiler::dnf2code(Tree cc)
{
    if (cc == gGlobal->nil) return "";
    Tree c1 = hd(cc);
    cc      = tl(cc);
    if (cc == gGlobal->nil) {
        return and2code(c1);
    } else {
        return subst("$0 || $1", and2code(c1), dnf2code(cc));
    }
}

string GraphCompiler::and2code(Tree cs)
{
    if (cs == gGlobal->nil) return "";
    Tree c1 = hd(cs);
    cs      = tl(cs);
    if (cs == gGlobal->nil) {
        return CS(c1);
    } else {
        return subst("$0 && $1", CS(c1), and2code(cs));
    }
}

string GraphCompiler::cnf2code(Tree cs)
{
    if (cs == gGlobal->nil) return "";
    Tree c1 = hd(cs);
    cs      = tl(cs);
    if (cs == gGlobal->nil) {
        return or2code(c1);
    } else {
        return subst("($0) && $1", or2code(c1), cnf2code(cs));
    }
}

string GraphCompiler::or2code(Tree cs)
{
    if (cs == gGlobal->nil) return "";
    Tree c1 = hd(cs);
    cs      = tl(cs);
    if (cs == gGlobal->nil) {
        return CS(c1);
    } else {
        return subst("$0 || $1", CS(c1), or2code(cs));
    }
}

#if _DNF_
#define CND2CODE dnf2code
#else
#define CND2CODE cnf2code
#endif

// temporary implementation for test purposes
string GraphCompiler::getConditionCode(Tree sig)
{
    Tree cc = fConditionProperty[sig];
    if ((cc != nullptr) && (cc != gGlobal->nil)) {
        return CND2CODE(cc);
    } else {
        return "";
    }
}

/*****************************************************************************
 CS : compile a signal
 *****************************************************************************/

/**
 * Test if a signal is already compiled
 * @param sig the signal expression to compile.
 * @param cexp the string representing the compiled expression.
 * @return true is already compiled
 */
bool GraphCompiler::getCompiledExpression(Tree sig, string& cexp)
{
    return fCompileProperty.get(sig, cexp);
}

/**
 * Set the string of a compiled expression is already compiled
 * @param sig the signal expression to compile.
 * @param cexp the string representing the compiled expression.
 * @return the cexp (for commodity)
 */
string GraphCompiler::setCompiledExpression(Tree sig, const string& cexp)
{
    // cerr << "GraphCompiler::setCompiledExpression : " << cexp << " ==> " << ppsig(sig) << endl;
    string old;
    if (fCompileProperty.get(sig, old) && (old != cexp)) {
        // cerr << "ERROR already a compiled expression attached : " << old << " replaced by " << cexp << endl;
        // exit(1);
    }
    fCompileProperty.set(sig, cexp);
    return cexp;
}

/*****************************************************************************
 vector name property
 *****************************************************************************/

/**
 * Set the vector name property of a signal, the name of the vector used to
 * store the previous values of the signal to implement a delay.
 * @param sig the signal expression.
 * @param vecname the string representing the vector name.
 * @return true is already compiled
 */
void GraphCompiler::setVectorNameProperty(Tree sig, const string& vecname)
{
    faustassert(vecname.size() > 0);
    fVectorProperty.set(sig, vecname);
}

/**
 * Get the vector name property of a signal, the name of the vector used to
 * store the previous values of the signal to implement a delay.
 * @param sig the signal expression.
 * @param vecname the string where to store the vector name.
 * @return true if the signal has this property, false otherwise
 */

bool GraphCompiler::getVectorNameProperty(Tree sig, string& vecname)
{
    return fVectorProperty.get(sig, vecname);
}

/**
 * Compile a signal
 * @param sig the signal expression to compile.
 * @return the C code translation of sig as a string
 */
string GraphCompiler::CS(Tree sig)
{
    // contextor contextRecursivness;
    string code;

    if (!getCompiledExpression(sig, code)) {
        // not compiled yet
        /*
         if (getRecursivness(sig) != contextRecursivness.get()) {
         contextRecursivness.set(getRecursivness(sig));
         }
         */
        code = generateCode(sig);
        setCompiledExpression(sig, code);
    }
    return code;
}

/*****************************************************************************
 compileMultiSignal
 *****************************************************************************/
static string nature2ctype(int n)
{
    string ctype{(n == kInt) ? "int" : ifloat()};
    return ctype;
}

/**
 * @brief Fill all needed information for table initialisation, in particular the
 * fTableInitGraph, an acyclic graph that describes in which order to initialize
 * tables.
 *
 * @param I the instruction set to analyze
 */
void GraphCompiler::tableDependenciesGraph(const set<Tree>& I)
{
    set<Tree> TID;                     // Treated IDs so far
    set<Tree> C = collectTableIDs(I);  // Remaining to be treated
    set<Tree> R = C;                   // Remaining to be treated
    while (!R.empty()) {
        set<Tree> N;                  // Set of unseen IDs
        for (Tree id : R) {           // for each table ID remaining to treat
            fTableInitGraph.add(id);  // add it to the table init graph
            TID.insert(id);           // remember it has been treated

            // get the init expression (how to compute the initial content of the table) of table id
            Tree init;
            faustassert(fTableInitExpression.get(id, init));  //

            // convert this init expression into a set J of instructions and a scheduling
            set<Tree> J;
            if (!fTableInitInstructions.get(init, J)) {
                J = expression2Instructions(init);
                fTableInitInstructions.set(init, J);
                fTableInitScheduling.set(init, schedule(J));
            }

            // compute the set D of tables needed to initialise id
            set<Tree> D = collectTableIDs(J);
            for (Tree dst : D) {
                fTableInitGraph.add(id, dst);
                if ((TID.count(dst) == 0) || (R.count(dst) == 0)) {
                    N.insert(dst);  // dst is unseen
                }
            }
        }
        R = N;  // Unseen are remaining to treat
    }

    // we can now compute the initialization order of the tables
    vector<Tree> S = serialize(fTableInitGraph);
    // cerr << "Table order" << endl;
    for (Tree id : S) {
        Tree       init;
        int        tblsize;
        int        nature;
        Scheduling s;

        faustassert(fTableInitExpression.get(id, init));
        faustassert(fTableInitSize.get(id, tblsize));
        faustassert(fTableInitNature.get(id, nature));
        faustassert(fTableInitScheduling.get(init, s));

        // cerr << "table " << *id << " has init expression " << ppsig(init) << endl;
        // cerr << s << endl;
        // Klass* k = new SigFloatGenKlass(nullptr, tree2str(id));
        Klass* k = nullptr;
        if (nature == kInt) {
            k = new SigIntFillMethod(nullptr, tree2str(id));
        } else {
            k = new SigFloatFillMethod(nullptr, tree2str(id));
        }
        SchedulingToMethod(s, C, k);
        fClass->addMethod(k);
        string tmp = subst("fill$0($1, $2);", k->getClassName(), T(tblsize), tree2str(id));
        // cerr << tmp << endl;
        fClass->addClearCode(tmp);
        // cerr << "The corresponding Klass:" << endl;
        // k->println(1, cerr);
        // cerr << "\n\n" << endl;
    }
}
/**
 * @brief Transform a scheduling into a C++ class
 *
 * @param name
 * @param super
 * @param numInputs
 * @param numOutputs
 * @param S
 * @return Klass
 */
void GraphCompiler::SchedulingToClass(Scheduling& S, Klass* K)
{
    for (int i = 0; i < K->inputs(); i++) {
        K->addZone3(subst("$1* input$0 = input[$0];", T(i), xfloat()));
    }
    for (int i = 0; i < K->outputs(); i++) {
        K->addZone3(subst("$1* output$0 = output[$0];", T(i), xfloat()));
    }

    K->addDeclCode("int \ttime;");
    K->addClearCode("time = 0;");
    K->addPostCode(Statement("", "++time;"));

    for (Tree i : S.fInitLevel) {
        // We compile
        Tree sig = S.fDic[i];
        Tree id, origin, content;
        int  nature;

        faustassert(isSigInstructionControlWrite(sig, id, origin, &nature, content));
        // force type annotation of transformed expressions
        Type ty = getSimpleType(content);

        string ctype{(nature == kInt) ? "int" : "float"};
        string vname{tree2str(id)};

        K->addDeclCode(subst("$0 \t$1;", ctype, vname));
        K->addInitCode(subst("$0 = $1;", vname, CS(content)));
    }

    for (Tree i : S.fBlockLevel) {
        // We compile
        Tree sig = S.fDic[i];
        Tree id, origin, content;
        int  nature;

        faustassert(isSigInstructionControlWrite(sig, id, origin, &nature, content));
        Type ty = getSimpleType(content);

        string ctype{(nature == kInt) ? "int" : "float"};
        string vname{tree2str(id)};

        K->addFirstPrivateDecl(vname);
        K->addZone2(subst("$0 \t$1 = $2;", nature2ctype(nature), vname, CS(content)));
    }

    for (Tree instr : S.fExecLevel) {
        // We compile
        Tree sig = S.fDic[instr];

        Tree id, origin, content, init, idx;
        int  i, nature, dmax, tblsize;

        if (isSigInstructionSharedWrite(sig, id, origin, &nature, content)) {
            Type   ty = getSimpleType(content);
            string vname{tree2str(id)};
            K->addExecCode(Statement("", subst("$0 \t$1 = $2;", nature2ctype(nature), vname, CS(content))));

        } else if (isSigInstructionTableWrite(sig, id, origin, &nature, &tblsize, init, idx, content)) {
            Type ty = getSimpleType(content);
            Type tz = getSimpleType(idx);

            string vname{tree2str(id)};
            K->addDeclCode(subst("$0 \t$1[$2];", nature2ctype(nature), vname, T(tblsize)));
            if (isZero(init)) {
                K->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(tblsize)));
            } else {
                // cerr << "Table init needed here for " << *id << endl;
            }
            if (!isNil(idx)) K->addExecCode(Statement("", subst("$0[$1] = $2;", vname, CS(idx), CS(content))));

        } else if (isSigOutput(sig, &i, content)) {
            K->addExecCode(Statement("", subst("output$0[i] = $1$2;", T(i), xcast(), CS(content))));

        } else {
            std::cerr << "ERROR, not a valid sample instruction : " << ppsig(sig) << endl;
            faustassert(false);
        }
    }
}
/**
 * @brief Transforms a scheduling into method (a special klass)
 *
 * @param S the instruction scheduling
 * @param C a set of ID
 * @param K
 */
void GraphCompiler::SchedulingToMethod(Scheduling& S, set<Tree>& C, Klass* K)
{
    // for (int i = 0; i < K->inputs(); i++) {
    //     K->addZone3(subst("$1* input$0 = input[$0];", T(i), xfloat()));
    // }
    // for (int i = 0; i < K->outputs(); i++) {
    //     K->addZone3(subst("$1* output$0 = output[$0];", T(i), xfloat()));
    // }

    K->addDeclCode("int \ttime;");
    K->addClearCode("time = 0;");
    K->addPostCode(Statement("", "++time;"));

    for (Tree i : S.fInitLevel) {
        // We compile
        Tree sig = S.fDic[i];
        Tree id, origin, content;
        int  nature;

        faustassert(isSigInstructionControlWrite(sig, id, origin, &nature, content));

        string ctype{(nature == kInt) ? "int" : "float"};
        string vname{tree2str(id)};

        K->addDeclCode(subst("$0 \t$1;", ctype, vname));
        K->addInitCode(subst("$0 = $1;", vname, CS(content)));
    }

    for (Tree i : S.fBlockLevel) {
        // We compile
        Tree sig = S.fDic[i];
        Tree id, origin, content;
        int  nature;

        faustassert(isSigInstructionControlWrite(sig, id, origin, &nature, content));

        string ctype{(nature == kInt) ? "int" : "float"};
        string vname{tree2str(id)};

        K->addFirstPrivateDecl(vname);
        K->addZone2(subst("$0 \t$1 = $2;", nature2ctype(nature), vname, CS(content)));
    }

    for (Tree instr : S.fExecLevel) {
        // We compile
        Tree sig = S.fDic[instr];

        Tree id, origin, content, init, idx;
        int  i, nature, dmax, tblsize;

        if (isSigInstructionSharedWrite(sig, id, origin, &nature, content)) {
            string vname{tree2str(id)};
            K->addExecCode(Statement("", subst("$0 \t$1 = $2;", nature2ctype(nature), vname, CS(content))));

        } else if (isSigInstructionTableWrite(sig, id, origin, &nature, &tblsize, init, idx, content)) {
            string vname{tree2str(id)};
            fClass->addDeclCode(subst("$0 \t$1[$2];", nature2ctype(nature), vname, T(tblsize)));
            Tree iexp;  // init expression
            faustassert(isSigGen(init, iexp));
            if (isZero(iexp)) {
                fClass->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(tblsize)));
            } else {
                // cerr << "Instruction Table Write " << *id << " has init " << ppsig(init) << endl;
            }
            if (!isNil(idx)) K->addExecCode(Statement("", subst("$0[$1] = $2;", vname, CS(idx), CS(content))));

        } else if (isSigOutput(sig, &i, content)) {
            K->addExecCode(Statement("", subst("output$0[i] = $1$2;", T(i), xcast(), CS(content))));

        } else {
            std::cerr << "ERROR, not a valid sample instruction : " << ppsig(sig) << endl;
            faustassert(false);
        }
    }
}

void GraphCompiler::compileMultiSignal(Tree L)
{
    L                = prepare(L);  // optimize, share and annotate expressions
    set<Tree>  INSTR = ExpressionsListToInstructionsSet(L);
    Scheduling S     = schedule(INSTR);

    SchedulingToClass(S, fClass);
    tableDependenciesGraph(INSTR);

    generateMetaData();
    generateUserInterfaceTree(prepareUserInterfaceTree(fUIRoot), true);
    generateMacroInterfaceTree("", prepareUserInterfaceTree(fUIRoot));
    if (fDescription) {
        fDescription->ui(prepareUserInterfaceTree(fUIRoot));
    }

    if (gGlobal->gPrintJSONSwitch) {
        ofstream xout(subst("$0.json", gGlobal->makeDrawPath()).c_str());
        xout << fJSON.JSON();
    }
}

/*****************************************************************************
 compileSingleSignal
 *****************************************************************************/

void GraphCompiler::compileSingleSignal(Tree sig)
{
    // contextor recursivness(0);
    sig = prepare2(sig);  // optimize and annotate expression
    fClass->addExecCode(Statement("", subst("output[i] = $0;", CS(sig))));
    generateUserInterfaceTree(prepareUserInterfaceTree(fUIRoot), true);
    generateMacroInterfaceTree("", prepareUserInterfaceTree(fUIRoot));
    if (fDescription) {
        fDescription->ui(prepareUserInterfaceTree(fUIRoot));
    }
}

/**
 * @brief generate sigTime 'time' variable code
 *
 */
void GraphCompiler::generateTime()
{
    fClass->addDeclCode("int \ttime;");
    fClass->addClearCode("time = 0;");
    fClass->addPostCode(Statement("", "++time;"));
}

/*****************************************************************************
 generateCode : dispatch according to signal
 *****************************************************************************/
/**
 * Main code generator dispatch.
 * @param sig the signal expression to compile.
 * @return the C code translation of sig
 */

string GraphCompiler::generateCode(Tree sig)
{
#if 0
    fprintf(stderr, "CALL generateCode(");
    printSignal(sig, stderr);
    fprintf(stderr, ")\n");
#endif

    int    i, nature, dmin;
    double r;
    Tree   c, sel, x, y, z, label, id, ff, largs, type, name, file, sf;
    Tree   origin, idx;

    // printf("compilation of %p : ", sig); print(sig); printf("\n");

    if (getUserData(sig)) {
        return generateXtended(sig);
    } else if (isSigTime(sig)) {
        return "time";
    } else if (isSigInstructionTableRead(sig, id, origin, &nature, &dmin, idx)) {
        return subst("$0[$1]", tree2str(id), CS(idx));
    } else if (isSigInstructionSharedRead(sig, id, origin, &nature)) {
        return tree2str(id);
    } else if (isSigInstructionControlRead(sig, id, origin, &nature)) {
        return tree2str(id);

    } else if (isSigInt(sig, &i)) {
        return T(i);
    } else if (isSigReal(sig, &r)) {
        return T(r) + inumix();
    } else if (isSigWaveform(sig)) {
        return generateWaveform(sig);
    } else if (isSigInput(sig, &i)) {
        return generateInput(sig, T(i));
    } else if (isSigOutput(sig, &i, x)) {
        return generateOutput(sig, T(i), CS(x));
    }

    else if (isSigBinOp(sig, &i, x, y)) {
        return generateBinOp(sig, i, x, y);
    } else if (isSigFFun(sig, ff, largs)) {
        return generateFFun(sig, ff, largs);
    } else if (isSigFConst(sig, type, name, file)) {
        return generateFConst(sig, tree2str(file), tree2str(name));
    } else if (isSigFVar(sig, type, name, file)) {
        return generateFVar(sig, tree2str(file), tree2str(name));
    }

    else if (isSigSelect2(sig, sel, x, y)) {
        return generateSelect2(sig, sel, x, y);
    } else if (isSigSelect3(sig, sel, x, y, z)) {
        return generateSelect3(sig, sel, x, y, z);
    }

    else if (isSigIntCast(sig, x)) {
        return generateIntCast(sig, x);
    } else if (isSigFloatCast(sig, x)) {
        return generateFloatCast(sig, x);
    }

    else if (isSigButton(sig, label)) {
        return generateButton(sig, label);
    } else if (isSigCheckbox(sig, label)) {
        return generateCheckbox(sig, label);
    } else if (isSigVSlider(sig, label, c, x, y, z)) {
        return generateVSlider(sig, label, c, x, y, z);
    } else if (isSigHSlider(sig, label, c, x, y, z)) {
        return generateHSlider(sig, label, c, x, y, z);
    } else if (isSigNumEntry(sig, label, c, x, y, z)) {
        return generateNumEntry(sig, label, c, x, y, z);
    }

    else if (isSigVBargraph(sig, label, x, y, z)) {
        return generateVBargraph(sig, label, x, y, CS(z));
    } else if (isSigHBargraph(sig, label, x, y, z)) {
        return generateHBargraph(sig, label, x, y, CS(z));
    }

    else if (isSigSoundfile(sig, label)) {
        return generateSoundfile(sig, label);
    } else if (isSigSoundfileLength(sig, sf, x)) {
        return generateCacheCode(sig, subst("$0cache->fLength[$1]", CS(sf), CS(x)));
    } else if (isSigSoundfileRate(sig, sf, x)) {
        return generateCacheCode(sig, subst("$0cache->fSR[$1]", CS(sf), CS(x)));
    } else if (isSigSoundfileBuffer(sig, sf, x, y, z)) {
        return generateCacheCode(sig,
                                 subst("$0cache->fBuffers[$1][$0cache->fOffset[$2]+$3]", CS(sf), CS(x), CS(y), CS(z)));
    }

    else if (isSigAttach(sig, x, y)) {
        CS(y);
        return generateCacheCode(sig, CS(x));
    } else if (isSigEnable(sig, x, y)) {
        return generateEnable(sig, x, y);
    }
    /* we should not have any control at this stage*/
    else {
        stringstream error;
        error << "ERROR when compiling, unrecognized signal : " << ppsig(sig) << endl;
        cerr << "\nERROR when compiling, unrecognized signal : " << ppsig(sig) << endl;
        throw faustexception(error.str());
    }
    return "error in generated code";
}

/*****************************************************************************
 NUMBERS
 *****************************************************************************/

// string GraphCompiler::generateNumber(Tree sig, const string& exp)
// {
//     return exp;
// }

/*****************************************************************************
 FOREIGN CONSTANTS
 *****************************************************************************/

string GraphCompiler::generateFConst(Tree sig, const string& file, const string& exp_aux)
{
    // Special case for 02/25/19 renaming
    string exp = (exp_aux == "fSamplingFreq") ? "fSampleRate" : exp_aux;

    string          ctype, vname;
    old_Occurences* o = fOccMarkup->retrieve(sig);

    addIncludeFile(file);

    return exp;
}

/*****************************************************************************
 FOREIGN VARIABLES
 *****************************************************************************/

string GraphCompiler::generateFVar(Tree sig, const string& file, const string& exp)
{
    string ctype, vname;

    addIncludeFile(file);
    return generateCacheCode(sig, exp);
}

/*****************************************************************************
 INPUTS - OUTPUTS
 *****************************************************************************/

string GraphCompiler::generateInput(Tree sig, const string& idx)
{
    if (gGlobal->gInPlace) {
        // TODO inputs must be cached for in-place transformations
        return subst("$1input$0[i]", idx, icast());
    } else {
        return subst("$1input$0[i]", idx, icast());
    }
}

string GraphCompiler::generateOutput(Tree sig, const string& idx, const string& arg)
{
    string dst = subst("output$0[i]", idx);
    fClass->addExecCode(Statement("", subst("$0 = $2$1;", dst, arg, xcast())));
    return dst;
}

/*****************************************************************************
 BINARY OPERATION
 *****************************************************************************/

string GraphCompiler::generateBinOp(Tree sig, int opcode, Tree arg1, Tree arg2)
{
    if (opcode == kDiv) {
        // special handling for division, we always want a float division
        Type t1 = getSimpleType(arg1);
        Type t2 = getSimpleType(arg2);

        interval j = t2->getInterval();

        if (j.haszero()) {
            // potential division by zero
            // interval    i = t1->getInterval();
            // std::cerr << "WARNING : potential division by zero (" << i << "/" << j << ") in " << ppsig(sig) <<
            // std::endl;
        }

        if (t1->nature() == kInt && t2->nature() == kInt) {
            return generateCacheCode(
                sig, subst("($3($0) $1 $3($2))", CS(arg1), gBinOpTable[opcode]->fName, CS(arg2), ifloat()));
        } else if (t1->nature() == kInt && t2->nature() == kReal) {
            return generateCacheCode(sig,
                                     subst("($3($0) $1 $2)", CS(arg1), gBinOpTable[opcode]->fName, CS(arg2), ifloat()));
        } else if (t1->nature() == kReal && t2->nature() == kInt) {
            return generateCacheCode(sig,
                                     subst("($0 $1 $3($2))", CS(arg1), gBinOpTable[opcode]->fName, CS(arg2), ifloat()));
        } else {
            return generateCacheCode(sig,
                                     subst("($0 $1 $2)", CS(arg1), gBinOpTable[opcode]->fName, CS(arg2), ifloat()));
        }
    } else {
        return generateCacheCode(sig, subst("($0 $1 $2)", CS(arg1), gBinOpTable[opcode]->fName, CS(arg2)));
    }
}

/*****************************************************************************
 Primitive Operations
 *****************************************************************************/

string GraphCompiler::generateFFun(Tree sig, Tree ff, Tree largs)
{
    addIncludeFile(ffincfile(ff));  // printf("inc file %s\n", ffincfile(ff));
    addLibrary(fflibfile(ff));      // printf("lib file %s\n", fflibfile(ff));

    string code = ffname(ff);
    code += '(';
    string sep = "";
    for (int i = 0; i < ffarity(ff); i++) {
        code += sep;
        code += CS(nth(largs, i));
        sep = ", ";
    }
    code += ')';
    return generateCacheCode(sig, code);
}

/*****************************************************************************
 CACHE CODE
 *****************************************************************************/

void GraphCompiler::getTypedNames(Type t, const string& prefix, string& ctype, string& vname)
{
    if (t->nature() == kInt) {
        ctype = "int";
        vname = subst("i$0", getFreshID(prefix));
    } else {
        ctype = ifloat();
        vname = subst("f$0", getFreshID(prefix));
    }
}

string GraphCompiler::generateCacheCode(Tree sig, const string& exp)
{
    return exp;
}

/*****************************************************************************
 CASTING
 *****************************************************************************/

string GraphCompiler::generateIntCast(Tree sig, Tree x)
{
    return generateCacheCode(sig, subst("int($0)", CS(x)));
}

string GraphCompiler::generateFloatCast(Tree sig, Tree x)
{
    return generateCacheCode(sig, subst("$1($0)", CS(x), ifloat()));
}

/*****************************************************************************
 user interface elements
 *****************************************************************************/

string GraphCompiler::generateButton(Tree sig, Tree path)
{
    string varname = getFreshID("fbutton");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    fClass->addInitUICode(subst("$0 = 0.0;", varname));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // return generateCacheCode(sig, varname);
    return generateCacheCode(sig, subst("$1($0)", varname, ifloat()));
}

string GraphCompiler::generateCheckbox(Tree sig, Tree path)
{
    string varname = getFreshID("fcheckbox");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    fClass->addInitUICode(subst("$0 = 0.0;", varname));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // return generateCacheCode(sig, varname);
    return generateCacheCode(sig, subst("$1($0)", varname, ifloat()));
}

string GraphCompiler::generateVSlider(Tree sig, Tree path, Tree cur, Tree min, Tree max, Tree step)
{
    string varname = getFreshID("fslider");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    fClass->addInitUICode(subst("$0 = $1;", varname, T(tree2float(cur))));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // return generateCacheCode(sig, varname);
    return generateCacheCode(sig, subst("$1($0)", varname, ifloat()));
}

string GraphCompiler::generateHSlider(Tree sig, Tree path, Tree cur, Tree min, Tree max, Tree step)
{
    string varname = getFreshID("fslider");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    fClass->addInitUICode(subst("$0 = $1;", varname, T(tree2float(cur))));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // return generateCacheCode(sig, varname);
    return generateCacheCode(sig, subst("$1($0)", varname, ifloat()));
}

string GraphCompiler::generateNumEntry(Tree sig, Tree path, Tree cur, Tree min, Tree max, Tree step)
{
    string varname = getFreshID("fentry");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    fClass->addInitUICode(subst("$0 = $1;", varname, T(tree2float(cur))));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // return generateCacheCode(sig, varname);
    return generateCacheCode(sig, subst("$1($0)", varname, ifloat()));
}

string GraphCompiler::generateVBargraph(Tree sig, Tree path, Tree min, Tree max, const string& exp)
{
    string varname = getFreshID("fbargraph");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    Type t = getCertifiedSigType(sig);
    switch (t->variability()) {
        case kKonst:
            fClass->addInitUICode(subst("$0 = $1;", varname, exp));
            break;

        case kBlock:
            fClass->addZone2(subst("$0 = $1;", varname, exp));
            break;

        case kSamp:
            fClass->addExecCode(Statement(getConditionCode(sig), subst("$0 = $1;", varname, exp)));
            break;
    }

    // return varname;
    return generateCacheCode(sig, varname);
}

string GraphCompiler::generateHBargraph(Tree sig, Tree path, Tree min, Tree max, const string& exp)
{
    string varname = getFreshID("fbargraph");
    fClass->addDeclCode(subst("$1 \t$0;", varname, xfloat()));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    Type t = getCertifiedSigType(sig);
    switch (t->variability()) {
        case kKonst:
            fClass->addInitUICode(subst("$0 = $1;", varname, exp));
            break;

        case kBlock:
            fClass->addZone2(subst("$0 = $1;", varname, exp));
            break;

        case kSamp:
            fClass->addExecCode(Statement(getConditionCode(sig), subst("$0 = $1;", varname, exp)));
            break;
    }

    // return varname;
    return generateCacheCode(sig, varname);
}

string GraphCompiler::generateSoundfile(Tree sig, Tree path)
{
    string varname = getFreshID("fSoundfile");

    // SL
    // fClass->addIncludeFile("<atomic>");
    // fClass->addIncludeFile("\"faust/gui/soundfile.h\"");

    // SL
    // fClass->addDeclCode(subst("std::atomic<Soundfile*> \t$0;", varname));
    fClass->addDeclCode(subst("Soundfile* \t$0;", varname));

    // fClass->addDeclCode(subst("Soundfile* \t$0cache;", varname));
    addUIWidget(reverse(tl(path)), uiWidget(hd(path), tree(varname), sig));

    // SL
    fClass->addInitUICode(subst("if (uintptr_t($0) == 0) $0 = defaultsound;", varname));
    fClass->addFirstPrivateDecl(subst("$0cache", varname));

    // SL
    // fClass->addZone2(subst("Soundfile* $0cache = $0.exchange(nullptr);", varname));
    fClass->addZone2(subst("Soundfile* $0cache = $0;", varname));
    fClass->addZone4(subst("$0 = $0cache;", varname));
    return varname;
}

/*****************************************************************************
 TABLES
 *****************************************************************************/

/*----------------------------------------------------------------------------
                        sigGen : initial table content
----------------------------------------------------------------------------*/

string GraphCompiler::generateSigGen(Tree sig, Tree content)
{
    string klassname = getFreshID("SIG");
    string signame   = getFreshID("sig");

    fClass->addSubKlass(signal2klass(fClass, klassname, content));
    fClass->addInitCode(subst("$0 $1;", klassname, signame));
    fInstanceInitProperty.set(content, pair<string, string>(klassname, signame));

    return signame;
}

string GraphCompiler::generateStaticSigGen(Tree sig, Tree content)
{
    string klassname = getFreshID("SIG");
    string signame   = getFreshID("sig");

    fClass->addSubKlass(signal2klass(fClass, klassname, content));
    fClass->addStaticInitCode(subst("$0 $1;", klassname, signame));
    fStaticInitProperty.set(content, pair<string, string>(klassname, signame));

    return signame;
}

/*----------------------------------------------------------------------------
                        sigTable : table declaration
----------------------------------------------------------------------------*/

string GraphCompiler::generateTable(Tree sig, Tree tsize, Tree content)
{
    int size;
    if (!isSigInt(tsize, &size)) {
        stringstream error;
        error << "ERROR in generateTable : " << *tsize << " is not an integer expression " << endl;
        throw faustexception(error.str());
    }

    string generator(CS(content));
    Tree   g;
    string cexp;
    string ctype, vname;

    // already compiled but check if we need to add declarations
    faustassert(isSigGen(content, g));
    pair<string, string> kvnames;
    if (!fInstanceInitProperty.get(g, kvnames)) {
        // not declared here, we add a declaration
        bool b = fStaticInitProperty.get(g, kvnames);
        faustassert(b);
        fClass->addInitCode(subst("$0 $1;", kvnames.first, kvnames.second));
    }

    // definition du nom et du type de la table
    // A REVOIR !!!!!!!!!
    Type t = getCertifiedSigType(content);  //, tEnv);
    if (t->nature() == kInt) {
        vname = getFreshID("itbl");
        ctype = "int";
    } else {
        vname = getFreshID("ftbl");
        ctype = ifloat();
    }

    // declaration de la table
    fClass->addDeclCode(subst("$0 \t$1[$2];", ctype, vname, T(size)));

    // initialisation du generateur de contenu
    fClass->addInitCode(subst("$0.init(sample_rate);", generator));
    // remplissage de la table
    fClass->addInitCode(subst("$0.fill($1,$2);", generator, T(size), vname));

    // on retourne le nom de la table
    return vname;
}

string GraphCompiler::generateStaticTable(Tree sig, Tree tsize, Tree content)
{
    int size;
    if (!isSigInt(tsize, &size)) {
        stringstream error;
        error << "ERROR in generateStaticTable : " << *tsize << " is not an integer expression " << endl;
        throw faustexception(error.str());
    }

    Tree   g;
    string cexp;
    string ctype, vname;

    faustassert(isSigGen(content, g));

    if (!getCompiledExpression(content, cexp)) {
        cexp = setCompiledExpression(content, generateStaticSigGen(content, g));
    } else {
        // already compiled but check if we need to add declarations
        pair<string, string> kvnames;
        if (!fStaticInitProperty.get(g, kvnames)) {
            // not declared here, we add a declaration
            bool b = fInstanceInitProperty.get(g, kvnames);
            faustassert(b);
            fClass->addStaticInitCode(subst("$0 $1;", kvnames.first, kvnames.second));
        }
    }

    // definition du nom et du type de la table
    // A REVOIR !!!!!!!!!
    Type t = getCertifiedSigType(content);  //, tEnv);
    if (t->nature() == kInt) {
        vname = getFreshID("itbl");
        ctype = "int";
    } else {
        vname = getFreshID("ftbl");
        ctype = ifloat();
    }

    // declaration de la table
    if (gGlobal->gMemoryManager) {
        fClass->addDeclCode(subst("static $0* \t$1;", ctype, vname));
        fClass->addStaticFields(subst("$0* \t$1::$2 = 0;", ctype, fClass->getClassName(), vname));
        fClass->addStaticInitCode(
            subst("$0 = static_cast<$1*>(fManager->allocate(sizeof($1) * $2));", vname, ctype, T(size)));
        fClass->addStaticDestroyCode(subst("fManager->destroy($0);", vname));
    } else {
        fClass->addDeclCode(subst("static $0 \t$1[$2];", ctype, vname, T(size)));
        fClass->addStaticFields(subst("$0 \t$1::$2[$3];", ctype, fClass->getClassName(), vname, T(size)));
    }

    // initialisation du generateur de contenu
    fClass->addStaticInitCode(subst("$0.init(sample_rate);", cexp));
    // remplissage de la table
    fClass->addStaticInitCode(subst("$0.fill($1,$2);", cexp, T(size), vname));

    // on retourne le nom de la table
    return vname;
}

/*----------------------------------------------------------------------------
                        sigWRTable : table assignement
----------------------------------------------------------------------------*/

string GraphCompiler::generateWRTbl(Tree sig, Tree tbl, Tree idx, Tree data)
{
    string tblName(CS(tbl));
    fClass->addExecCode(Statement(getConditionCode(sig), subst("$0[$1] = $2;", tblName, CS(idx), CS(data))));
    return tblName;
}

/*----------------------------------------------------------------------------
                        sigRDTable : table access
----------------------------------------------------------------------------*/

string GraphCompiler::generateRDTbl(Tree sig, Tree tbl, Tree idx)
{
    // YO le 21/04/05 : La lecture des tables n'était pas mise dans le cache
    // et donc le code était dupliqué (dans tester.dsp par exemple)
    // return subst("$0[$1]", CS(tEnv, tbl), CS(tEnv, idx));

    // cerr << "generateRDTable " << *sig << endl;
    // test the special case of a read only table that can be compiled as a static member
    Tree id, size, content;
    if (isSigTable(tbl, id, size, content)) {
        string tblname;
        if (!getCompiledExpression(tbl, tblname)) {
            tblname = setCompiledExpression(tbl, generateStaticTable(tbl, size, content));
        }
        return generateCacheCode(sig, subst("$0[$1]", tblname, CS(idx)));
    } else {
        return generateCacheCode(sig, subst("$0[$1]", CS(tbl), CS(idx)));
    }
}

/*****************************************************************************
 PREFIX, DELAY A PREFIX VALUE
 *****************************************************************************/

string GraphCompiler::generateEnable(Tree sig, Tree x, Tree y)
{
    CS(y);
    return generateCacheCode(x, CS(x));
    // return CS(x);
}

string GraphCompiler::generatePrefix(Tree sig, Tree x, Tree e)
{
    Type te = getCertifiedSigType(sig);  //, tEnv);

    string vperm = getFreshID("M");
    string vtemp = getFreshID("T");

    string type = old_cType(te);

    fClass->addDeclCode(subst("$0 \t$1;", type, vperm));
    fClass->addInitCode(subst("$0 = $1;", vperm, CS(x)));
    fClass->addInitCode(subst("$0 \t$1;", type, vtemp));

    fClass->addExecCode(Statement(getConditionCode(sig), subst("$0 = $1;", vtemp, vperm)));
    fClass->addExecCode(Statement(getConditionCode(sig), subst("$0 = $1;", vperm, CS(e))));
    return vtemp;
}

/*****************************************************************************
 IOTA(n)
 *****************************************************************************/

static bool isPowerOf2(int n)
{
    return !(n & (n - 1));
}

string GraphCompiler::generateIota(Tree sig, Tree n)
{
    int size;
    if (!isSigInt(n, &size)) {
        throw faustexception("ERROR in generateIota\n");
    }

    string vperm = getFreshID("iota");

    fClass->addDeclCode(subst("int \t$0;", vperm));
    fClass->addClearCode(subst("$0 = 0;", vperm));

    if (isPowerOf2(size)) {
        fClass->addExecCode(Statement("", subst("$0 = ($0+1)&$1;", vperm, T(size - 1))));
    } else {
        fClass->addExecCode(Statement("", subst("if (++$0 == $1) $0=0;", vperm, T(size))));
    }
    return vperm;
}

/*****************************************************************************
 SELECT
 *****************************************************************************/

string GraphCompiler::generateSelect2(Tree sig, Tree sel, Tree s1, Tree s2)
{
    return generateCacheCode(sig, subst("(($0)?$1:$2)", CS(sel), CS(s2), CS(s1)));
}

/**
 * Generate a select3 code (using if-then-else)
 * ((int n = sel==0)? s0 : ((sel==1)? s1 : s2))
 * int nn; ((nn=sel) ? ((nn==1)? s1 : s2) : s0);
 */
string GraphCompiler::generateSelect3(Tree sig, Tree sel, Tree s1, Tree s2, Tree s3)
{
    return generateCacheCode(sig, subst("(($0==0)? $1 : (($0==1)?$2:$3) )", CS(sel), CS(s1), CS(s2), CS(s3)));
}

#if 0
string GraphCompiler::generateSelect3(Tree sig, Tree sel, Tree s1, Tree s2, Tree s3)
{
    Type t = getCertifiedSigType(sig);
    Type t1 = getCertifiedSigType(s1);
    Type t2 = getCertifiedSigType(s2);
    Type t3 = getCertifiedSigType(s3);
    Type w = min(t1,min(t2,t3));

    string type = old_cType(t);
    string var = getFreshID("S");

    switch (w->variability())
    {
        case kKonst:
            fClass->addDeclCode(subst("$0 \t$1[3];", type, var));
            break;
        case kBlock:
            //fClass->addLocalDecl(type, subst("$0[3]", var));
            //fClass->addLocalVecDecl(type, var, 3);
            fClass->addSharedDecl(var);
            fClass->addZone1(subst("$0 \t$1[3];", type, var));
            break;
        case kSamp:
            fClass->addExecCode(subst("$0 \t$1[3];", type, var));
            break;
    }

    switch (t1->variability())
    {
        case kKonst:
            fClass->addClearCode(subst("$0[0] = $1;", var, CS(s1)));
            break;
        case kBlock:
            fClass->addZone2b(subst("$0[0] = $1;", var, CS(s1)));
            break;
        case kSamp:
            fClass->addExecCode(subst("$0[0] = $1;", var, CS(s1)));
            break;
    }

    switch (t2->variability())
    {
        case kKonst:
            fClass->addClearCode(subst("$0[1] = $1;", var, CS(s2)));
            break;
        case kBlock:
            fClass->addZone2b(subst("$0[1] = $1;", var, CS(s2)));
            break;
        case kSamp:
            fClass->addExecCode(subst("$0[1] = $1;", var, CS(s2)));
            break;
    }

    switch (t3->variability())
    {
        case kKonst:
            fClass->addClearCode(subst("$0[2] = $1;", var, CS(s3)));
            break;
        case kBlock:
            fClass->addZone2b(subst("$0[2] = $1;", var, CS(s3)));
            break;
        case kSamp:
            fClass->addExecCode(subst("$0[2] = $1;", var, CS(s3)));
            break;
    }

    return generateCacheCode(sig, subst("$0[$1]", var, CS(sel)));
}
#endif

/*****************************************************************************
 EXTENDED
 *****************************************************************************/

string GraphCompiler::generateXtended(Tree sig)
{
    auto*          p = (xtended*)getUserData(sig);
    vector<string> args;
    vector<Type>   types;

    for (int i = 0; i < sig->arity(); i++) {
        args.push_back(CS(sig->branch(i)));
        types.push_back(getSimpleType(sig->branch(i)));
    }

    return p->old_generateCode(fClass, args, types);
}

/**
 * Compute the minimal power of 2 greater than x
 */

int GraphCompiler::pow2limit(int x)
{
    int n = 2;
    while (n < x) {
        n = 2 * n;
    }
    return n;
}

/*****************************************************************************
 N-SAMPLE FIXED DELAY : sig = exp@delay

 case 1-sample max delay :
 Y(t-0)	Y(t-1)
 Temp	Var                     gLessTempSwitch = false
 V[0]	V[1]                    gLessTempSwitch = true

 case max delay < gMaxCopyDelay :
 Y(t-0)	Y(t-1)	Y(t-2)  ...
 Temp	V[0]	V[1]	...     gLessTempSwitch = false
 V[0]	V[1]	V[2]	...     gLessTempSwitch = true

 case max delay >= gMaxCopyDelay :
 Y(t-0)	Y(t-1)	Y(t-2)  ...
 Temp	V[0]	V[1]	...
 V[0]	V[1]	V[2]	...


 *****************************************************************************/

// /**
//  * Generate code for accessing a delayed signal. The generated code depend of
//  * the maximum delay attached to exp and the gLessTempSwitch.
//  */

// string GraphCompiler::generateFixDelay(Tree sig, Tree exp, Tree delay)
// {
//     // cerr << "GraphCompiler::generateFixDelay sig = " << *sig << endl;
//     // cerr << "GraphCompiler::generateFixDelay exp = " << *exp << endl;
//     // cerr << "GraphCompiler::generateFixDelay del = " << *delay << endl;

//     string code = CS(exp);  // ensure exp is compiled to have a vector name
//     int    mxd  = fOccMarkup->retrieve(exp)->getMaxDelay();
//     string vecname;

//     if (!getVectorNameProperty(exp, vecname)) {
//         if (mxd == 0) {
//             // cerr << "it is a pure zero delay : " << code << endl;
//             return code;
//         } else {
//             cerr << "No vector name for : " << ppsig(exp) << endl;
//             faustassert(0);
//         }
//     }

//     if (mxd == 0) {
//         // not a real vector name but a scalar name
//         return vecname;

//     } else if (mxd < gGlobal->gMaxCopyDelay) {
//         int d;
//         if (isSigInt(delay, &d)) {
//             return subst("$0[$1]", vecname, CS(delay));
//         } else {
//             return generateCacheCode(sig, subst("$0[$1]", vecname, CS(delay)));
//         }

//     } else {
//         // long delay : we use a ring buffer of size 2^x
//         int N = pow2limit(mxd + 1);
//         return generateCacheCode(sig, subst("$0[(IOTA-$1)&$2]", vecname, CS(delay), T(N - 1)));
//     }
// }

// /**
//  * Generate code for the delay mecchanism. The generated code depend of the
//  * maximum delay attached to exp and the "less temporaries" switch
//  */

// string GraphCompiler::generateDelayVec(Tree sig, const string& exp, const string& ctype, const string& vname, int
// mxd)
// {
//     string s = generateDelayVecNoTemp(sig, exp, ctype, vname, mxd);
//     if (getCertifiedSigType(sig)->variability() < kSamp) {
//         return exp;
//     } else {
//         return s;
//     }
// }

// /**
//  * Generate code for the delay mecchanism without using temporary variables
//  */

// string GraphCompiler::generateDelayVecNoTemp(Tree sig, const string& exp, const string& ctype, const string& vname,
//                                              int mxd)
// {
//     faustassert(mxd > 0);

//     // bool odocc = fOccMarkup->retrieve(sig)->hasOutDelayOccurences();
//     string ccs = getConditionCode(sig);

//     if (mxd < gGlobal->gMaxCopyDelay) {
//         // short delay : we copy
//         fClass->addDeclCode(subst("$0 \t$1[$2];", ctype, vname, T(mxd + 1)));
//         fClass->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(mxd + 1)));
//         fClass->addExecCode(Statement(ccs, subst("$0[0] = $1;", vname, exp)));

//         // generate post processing copy code to update delay values
//         if (mxd == 1) {
//             fClass->addPostCode(Statement(ccs, subst("$0[1] = $0[0];", vname)));
//         } else if (mxd == 2) {
//             // fClass->addPostCode(subst("$0[2] = $0[1];", vname));
//             fClass->addPostCode(Statement(ccs, subst("$0[2] = $0[1]; $0[1] = $0[0];", vname)));
//         } else {
//             fClass->addPostCode(Statement(ccs, subst("for (int i=$0; i>0; i--) $1[i] = $1[i-1];", T(mxd), vname)));
//         }
//         setVectorNameProperty(sig, vname);
//         return subst("$0[0]", vname);

//     } else {
//         // generate code for a long delay : we use a ring buffer of size N = 2**x > mxd
//         int N = pow2limit(mxd + 1);

//         // we need a iota index
//         ensureIotaCode();

//         // declare and init
//         fClass->addDeclCode(subst("$0 \t$1[$2];", ctype, vname, T(N)));
//         fClass->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(N)));

//         // execute
//         fClass->addExecCode(Statement(ccs, subst("$0[IOTA&$1] = $2;", vname, T(N - 1), exp)));
//         setVectorNameProperty(sig, vname);
//         return subst("$0[IOTA&$1]", vname, T(N - 1));
//     }
// }

// /**
//  * Generate code for the delay mecchanism without using temporary variables
//  */

// void GraphCompiler::generateDelayLine(const string& ctype, const string& vname, int mxd, const string& exp,
//                                       const string& ccs)
// {
//     // faustassert(mxd > 0);
//     if (mxd == 0) {
//         // cerr << "MXD==0 :  " << vname << " := " << exp << endl;
//         // no need for a real vector
//         fClass->addExecCode(Statement(ccs, subst("$0 \t$1 = $2;", ctype, vname, exp)));

//     } else if (mxd < gGlobal->gMaxCopyDelay) {
//         // cerr << "small delay : " << vname << "[" << mxd << "]" << endl;

//         // short delay : we copy
//         fClass->addDeclCode(subst("$0 \t$1[$2];", ctype, vname, T(mxd + 1)));
//         fClass->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(mxd + 1)));
//         fClass->addExecCode(Statement(ccs, subst("$0[0] = $1;", vname, exp)));

//         // generate post processing copy code to update delay values
//         if (mxd == 1) {
//             fClass->addPostCode(Statement(ccs, subst("$0[1] = $0[0];", vname)));
//         } else if (mxd == 2) {
//             fClass->addPostCode(Statement(ccs, subst("$0[2] = $0[1]; $0[1] = $0[0];", vname)));
//         } else {
//             fClass->addPostCode(Statement(ccs, subst("for (int i=$0; i>0; i--) $1[i] = $1[i-1];", T(mxd), vname)));
//         }

//     } else {
//         // generate code for a long delay : we use a ring buffer of size N = 2**x > mxd
//         int N = pow2limit(mxd + 1);

//         // we need a iota index
//         ensureIotaCode();

//         // declare and init
//         fClass->addDeclCode(subst("$0 \t$1[$2];", ctype, vname, T(N)));
//         fClass->addClearCode(subst("for (int i=0; i<$1; i++) $0[i] = 0;", vname, T(N)));

//         // execute
//         fClass->addExecCode(Statement(ccs, subst("$0[IOTA&$1] = $2;", vname, T(N - 1), exp)));
//     }
// }

/**
 * Generate code for a unique IOTA variable increased at each sample
 * and used to index ring buffers.
 */
void GraphCompiler::ensureIotaCode()
{
    if (!fHasIota) {
        fHasIota = true;
        fClass->addDeclCode("int \tIOTA;");
        fClass->addClearCode("IOTA = 0;");
        fClass->addPostCode(Statement("", "IOTA = IOTA+1;"));
    }
}

/*****************************************************************************
 WAVEFORM
 *****************************************************************************/

/**
 * Generate code for a waveform. The waveform will be declared as a static field.
 * The name of the waveform is returned in vname and its size in size.
 */
void GraphCompiler::declareWaveform(Tree sig, string& vname, int& size)
{
    // computes C type and unique name for the waveform
    string ctype;
    getTypedNames(getCertifiedSigType(sig), "Wave", ctype, vname);

    size = sig->arity();

    // Converts waveform into a string : "{a,b,c,...}"
    stringstream content;

    char sep = '{';
    for (int i = 0; i < size; i++) {
        content << sep << ppsig(sig->branch(i));
        sep = ',';
    }
    content << '}';

    // Declares the Waveform
    fClass->addDeclCode(subst("static $0 \t$1[$2];", ctype, vname, T(size)));
    fClass->addDeclCode(subst("int \tidx$0;", vname));
    fClass->addInitCode(subst("idx$0 = 0;", vname));
    fClass->getTopParentKlass()->addStaticFields(
        subst("$0 \t$1::$2[$3] = ", ctype, fClass->getFullClassName(), vname, T(size)) + content.str() + ";");
}

string GraphCompiler::generateWaveform(Tree sig)
{
    string vname;
    int    size;

    declareWaveform(sig, vname, size);
    fClass->addPostCode(Statement(getConditionCode(sig), subst("idx$0 = (idx$0 + 1) % $1;", vname, T(size))));
    return generateCacheCode(sig, subst("$0[idx$0]", vname));
}

//------------------------------------------------------------------------------
// Create a specific property key for the sharing count of subtrees of t
//------------------------------------------------------------------------------

int GraphCompiler::getSharingCount(Tree sig)
{
    // cerr << "getSharingCount of : " << *sig << " = ";
    Tree c;
    if (getProperty(sig, fSharingKey, c)) {
        // cerr << c->node().getInt() << endl;
        return c->node().getInt();
    } else {
        // cerr << 0 << endl;
        return 0;
    }
}

void GraphCompiler::setSharingCount(Tree sig, int count)
{
    // cerr << "setSharingCount of : " << *sig << " <- " << count << endl;
    setProperty(sig, fSharingKey, tree(count));
}

//------------------------------------------------------------------------------
// Create a specific property key for the sharing count of subtrees of t
//------------------------------------------------------------------------------

void GraphCompiler::sharingAnalysis(Tree t)
{
    fSharingKey = shprkey(t);
    if (isList(t)) {
        while (isList(t)) {
            sharingAnnotation(kSamp, hd(t));
            t = tl(t);
        }
    } else {
        sharingAnnotation(kSamp, t);
    }
}

//------------------------------------------------------------------------------
// Create a specific property key for the sharing count of subtrees of t
//------------------------------------------------------------------------------
void GraphCompiler::sharingAnnotation(int vctxt, Tree sig)
{
    Tree c, x, y, z;

    // cerr << "START sharing annotation of " << *sig << endl;
    int count = getSharingCount(sig);

    if (count > 0) {
        // it is not our first visit
        setSharingCount(sig, count + 1);

    } else {
        // it is our first visit,
        int v = getCertifiedSigType(sig)->variability();

        // check "time sharing" cases
        if (v < vctxt) {
            setSharingCount(sig, 2);  // time sharing occurence : slower expression in faster context
        } else {
            setSharingCount(sig, 1);  // regular occurence
        }

        if (isSigSelect3(sig, c, y, x, z)) {
            // make a special case for select3 implemented with real if
            // because the c expression will be used twice in the C++
            // translation
            sharingAnnotation(v, c);
            sharingAnnotation(v, c);
            sharingAnnotation(v, x);
            sharingAnnotation(v, y);
            sharingAnnotation(v, z);
        } else {
            // Annotate the sub signals
            vector<Tree> subsig;
            int          n = getSubSignals(sig, subsig);
            if (n > 0 && !isSigGen(sig)) {
                for (int i = 0; i < n; i++) sharingAnnotation(v, subsig[i]);
            }
        }
    }
    // cerr << "END sharing annotation of " << *sig << endl;
}

//------------------------------------------------------------------------------
// Condition annotation due to enabled expressions
//------------------------------------------------------------------------------
#if 0
void GraphCompiler::conditionStatistics(Tree l)
{
    for (const auto& p : fConditionProperty) {
        fConditionStatistics[p.second]++;
    }
    std::cout << "\nConditions statistics" << std::endl;
    for (const auto& p : fConditionStatistics) {
        std::cout << ppsig(p.first) << ":" << p.second << std::endl;

    }
}
#endif

void GraphCompiler::conditionStatistics(Tree l)
{
    map<Tree, int> fConditionStatistics;  // used with the new X,Y:enable --> sigEnable(X*Y,Y>0) primitive
    for (const auto& p : fConditionProperty) {
        for (Tree lc = p.second; !isNil(lc); lc = tl(lc)) {
            fConditionStatistics[hd(lc)]++;
        }
    }
    std::cout << "\nConditions statistics" << std::endl;
    for (const auto& p : fConditionStatistics) {
        std::cout << ppsig(p.first) << ":" << p.second << std::endl;
    }
}

void GraphCompiler::conditionAnnotation(Tree l)
{
    while (isList(l)) {
        conditionAnnotation(hd(l), gGlobal->nil);
        l = tl(l);
    }
}
#if _DNF_

#define _OR_ dnfOr
#define _AND_ dnfAnd
#define _CND_ dnfCond

#else

#define _OR_ cnfOr
#define _AND_ cnfAnd
#define _CND_ cnfCond

#endif

void GraphCompiler::conditionAnnotation(Tree t, Tree nc)
{
    // Check if we need to annotate the tree with new conditions
    auto p = fConditionProperty.find(t);
    if (p != fConditionProperty.end()) {
        Tree cc = p->second;
        Tree xc = _OR_(cc, nc);
        if (cc == xc) {
            // Tree t already correctly annotated, nothing to change
            return;
        } else {
            // we need to re-annotate the tree with a new condition
            nc        = xc;
            p->second = nc;
        }
    } else {
        // first visit
        fConditionProperty[t] = nc;
    }

    // Annotate the subtrees with the new condition nc
    // which is either the nc passed as argument or nc <- (cc v nc)

    Tree x, y;
    if (isSigEnable(t, x, y)) {
        // specific annotation case for sigEnable
        conditionAnnotation(y, nc);
        conditionAnnotation(x, _AND_(nc, _CND_(y)));
    } else {
        // general annotation case
        // Annotate the sub signals with nc
        vector<Tree> subsig;
        int          n = getSubSignals(t, subsig);
        if (n > 0 && !isSigGen(t)) {
            for (int i = 0; i < n; i++) conditionAnnotation(subsig[i], nc);
        }
    }
}