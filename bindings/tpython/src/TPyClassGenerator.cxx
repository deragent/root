// Author: Enric Tejedor CERN  08/2019
// Original PyROOT code by Wim Lavrijsen, LBL
//
// /*************************************************************************
//  * Copyright (C) 1995-2019, Rene Brun and Fons Rademakers.               *
//  * All rights reserved.                                                  *
//  *                                                                       *
//  * For the licensing terms see $ROOTSYS/LICENSE.                         *
//  * For the list of contributors see $ROOTSYS/README/CREDITS.             *
//  *************************************************************************/

#include "Python.h"

#include "TPyClassGenerator.h"
#include "TPyReturn.h"

// ROOT
#include "TClass.h"
#include "TInterpreter.h"
#include "TROOT.h"
#include "TList.h"

// Standard
#include <sstream>
#include <string>
#include <typeinfo>

namespace {
   class PyGILRAII {
      PyGILState_STATE m_GILState;
   public:
      PyGILRAII() : m_GILState(PyGILState_Ensure()) { }
      ~PyGILRAII() { PyGILState_Release(m_GILState); }
   };
}

//- public members -----------------------------------------------------------
TClass *TPyClassGenerator::GetClass(const char *name, Bool_t load)
{
   // Just forward.
   return GetClass(name, load, kFALSE);
}

//- public members -----------------------------------------------------------
TClass *TPyClassGenerator::GetClass(const char *name, Bool_t load, Bool_t silent)
{
   // Class generator to make python classes available to Cling

   if (!load || !name)
      return 0;

   PyGILRAII thePyGILRAII;

   // first, check whether the name is of a module
   PyObject *modules = PySys_GetObject(const_cast<char *>("modules"));
   PyObject *pyname = PyUnicode_FromString(name);
   PyObject *keys = PyDict_Keys(modules);
   Bool_t isModule = PySequence_Contains(keys, pyname);
   Py_DecRef(keys);
   Py_DecRef(pyname);

   if (isModule) {
      // the normal TClass::GetClass mechanism doesn't allow direct returns, so
      // do our own check
      TClass *cl = (TClass *)gROOT->GetListOfClasses()->FindObject(name);
      if (cl)
         return cl;

      std::ostringstream nsCode;
      nsCode << "namespace " << name << " {\n";

      // add all free functions
      PyObject *bases = PyUnicode_FromString("__bases__");
      PyObject *mod = PyDict_GetItemString(modules, const_cast<char *>(name));
      PyObject *dct = PyModule_GetDict(mod);
      keys = PyDict_Keys(dct);

      for (int i = 0; i < PyList_GET_SIZE(keys); ++i) {
         PyObject *key = PyList_GET_ITEM(keys, i);
         Py_IncRef(key);

         PyObject *attr = PyDict_GetItem(dct, key);
         Py_IncRef(attr);

         // TODO: refactor the code below with the class method code
         if (PyCallable_Check(attr) && !(PyType_Check(attr) || PyObject_HasAttr(attr, bases))) {
            std::string func_name = PyUnicode_AsUTF8(key);

            // figure out number of variables required
#if PY_VERSION_HEX < 0x30d00f0
            PyObject *func_code = PyObject_GetAttrString(attr, (char *)"func_code");
#else
            PyObject *func_code = nullptr;
            PyObject_GetOptionalAttrString(attr, (char *)"func_code", &func_code);
#endif
            PyObject *var_names = func_code ? PyObject_GetAttrString(func_code, (char *)"co_varnames") : NULL;
            int nVars = var_names ? PyTuple_GET_SIZE(var_names) : 0 /* TODO: probably large number, all default? */;
            if (nVars < 0)
               nVars = 0;
            Py_DecRef(var_names);
            Py_DecRef(func_code);

            nsCode << " TPyReturn " << func_name << "(";
            for (int ivar = 0; ivar < nVars; ++ivar) {
               nsCode << "const TPyArg& a" << ivar;
               if (ivar != nVars - 1)
                  nsCode << ", ";
            }
            nsCode << ") {\n";
            nsCode << "  std::vector<TPyArg> v; v.reserve(" << nVars << ");\n";

            // add the variables
            for (int ivar = 0; ivar < nVars; ++ivar)
               nsCode << "  v.push_back(a" << ivar << ");\n";

            // call dispatch (method or class pointer hard-wired)
            nsCode << "  return TPyReturn(TPyArg::CallMethod((PyObject*)" << std::showbase << (uintptr_t)attr << ", v)); }\n";
         }

         Py_DecRef(attr);
         Py_DecRef(key);
      }

      Py_DecRef(keys);
      Py_DecRef(bases);

      nsCode << " }";

      if (gInterpreter->LoadText(nsCode.str().c_str())) {
         TClass *klass = new TClass(name, silent);
         TClass::AddClass(klass);
         return klass;
      }

      return nullptr;
   }

   // determine module and class name part
   std::string clName = name;
   std::string::size_type pos = clName.rfind('.');

   if (pos == std::string::npos)
      return 0; // this isn't a python style class

   std::string mdName = clName.substr(0, pos);
   clName = clName.substr(pos + 1, std::string::npos);

   // create class in namespace, if it exists (no load, silent)
   Bool_t useNS = gROOT->GetListOfClasses()->FindObject(mdName.c_str()) != 0;
   if (!useNS) {
      // the class itself may exist if we're using the global scope
      TClass *cl = (TClass *)gROOT->GetListOfClasses()->FindObject(clName.c_str());
      if (cl)
         return cl;
   }

   // locate and get class
   PyObject *mod = PyImport_AddModule(const_cast<char *>(mdName.c_str()));
   if (!mod) {
      PyErr_Clear();
      return 0; // module apparently disappeared
   }

   Py_IncRef(mod);
   PyObject *pyclass = PyDict_GetItemString(PyModule_GetDict(mod), const_cast<char *>(clName.c_str()));
   Py_IncRef(pyclass);
   Py_DecRef(mod);

   if (!pyclass) {
      PyErr_Clear(); // the class is no longer available?!
      return 0;
   }

   // get a listing of all python-side members
   PyObject *attrs = PyObject_Dir(pyclass);
   if (!attrs) {
      PyErr_Clear();
      Py_DecRef(pyclass);
      return 0;
   }

   // pre-amble Cling proxy class
   std::ostringstream proxyCode;
   if (useNS)
      proxyCode << "namespace " << mdName << " { ";
   proxyCode << "class " << clName << " {\nprivate:\n PyObject* fPyObject;\npublic:\n";

   // loop over and add member functions
   Bool_t hasConstructor = kFALSE, hasDestructor = kFALSE;
   for (int i = 0; i < PyList_GET_SIZE(attrs); ++i) {
      PyObject *label = PyList_GET_ITEM(attrs, i);
      Py_IncRef(label);
      PyObject *attr = PyObject_GetAttr(pyclass, label);

      // collect only member functions (i.e. callable elements in __dict__)
      if (PyCallable_Check(attr)) {
         std::string mtName = PyUnicode_AsUTF8(label);

         if (mtName == "__del__") {
            hasDestructor = kTRUE;
            proxyCode << " ~" << clName << "() { TPyArg::CallDestructor(fPyObject); }\n";
            continue;
         }

         Bool_t isConstructor = mtName == "__init__";
         if (!isConstructor && mtName.find("__", 0, 2) == 0)
            continue; // skip all other python special funcs

// figure out number of variables required
         PyObject *func_code = PyObject_GetAttrString(attr, "__code__");
         PyObject *var_names = func_code ? PyObject_GetAttrString(func_code, (char *)"co_varnames") : NULL;
         if (PyErr_Occurred())
            PyErr_Clear(); // happens for slots; default to 0 arguments

         int nVars =
            var_names ? PyTuple_GET_SIZE(var_names) - 1 /* self */ : 0 /* TODO: probably large number, all default? */;
         if (nVars < 0)
            nVars = 0;
         Py_DecRef(var_names);
         Py_DecRef(func_code);

         // method declaration as appropriate
         if (isConstructor) {
            hasConstructor = kTRUE;
            proxyCode << " " << clName << "(";
         } else // normal method
            proxyCode << " TPyReturn " << mtName << "(";
         for (int ivar = 0; ivar < nVars; ++ivar) {
            proxyCode << "const TPyArg& a" << ivar;
            if (ivar != nVars - 1)
               proxyCode << ", ";
         }
         proxyCode << ") {\n";
         proxyCode << "  std::vector<TPyArg> v; v.reserve(" << nVars + (isConstructor ? 0 : 1) << ");\n";

         // add the 'self' argument as appropriate
         if (!isConstructor)
            proxyCode << "  v.push_back(fPyObject);\n";

         // then add the remaining variables
         for (int ivar = 0; ivar < nVars; ++ivar)
            proxyCode << "  v.push_back(a" << ivar << ");\n";

         // call dispatch (method or class pointer hard-wired)
         if (!isConstructor)
            proxyCode << "  return TPyReturn(TPyArg::CallMethod((PyObject*)" << std::showbase << (uintptr_t)attr << ", v))";
         else
            proxyCode << "  TPyArg::CallConstructor(fPyObject, (PyObject*)" << std::showbase << (uintptr_t)pyclass << ", v)";
         proxyCode << ";\n }\n";
      }

      // no decref of attr for now (b/c of hard-wired ptr); need cleanup somehow
      Py_DecRef(label);
   }

   // special case if no constructor or destructor
   if (!hasConstructor)
      proxyCode << " " << clName << "() {\n TPyArg::CallConstructor(fPyObject, (PyObject*)" << std::showbase << (uintptr_t)pyclass
                << "); }\n";

   if (!hasDestructor)
      proxyCode << " ~" << clName << "() { TPyArg::CallDestructor(fPyObject); }\n";

   // for now, don't allow copying (ref-counting wouldn't work as expected anyway)
   proxyCode << " " << clName << "(const " << clName << "&) = delete;\n";
   proxyCode << " " << clName << "& operator=(const " << clName << "&) = delete;\n";

   // closing and building of Cling proxy class
   proxyCode << "};";
   if (useNS)
      proxyCode << " }";

   Py_DecRef(attrs);
   // done with pyclass, decref here, assuming module is kept
   Py_DecRef(pyclass);

   // body compilation
   if (!gInterpreter->LoadText(proxyCode.str().c_str()))
      return nullptr;

   // done, let ROOT manage the new class
   TClass *klass = new TClass(useNS ? (mdName + "::" + clName).c_str() : clName.c_str(), silent);
   TClass::AddClass(klass);

   return klass;
}

////////////////////////////////////////////////////////////////////////////////
/// Just forward; based on type name only.

TClass *TPyClassGenerator::GetClass(const std::type_info &typeinfo, Bool_t load, Bool_t silent)
{
   return GetClass(typeinfo.name(), load, silent);
}

////////////////////////////////////////////////////////////////////////////////
/// Just forward; based on type name only

TClass *TPyClassGenerator::GetClass(const std::type_info &typeinfo, Bool_t load)
{
   return GetClass(typeinfo.name(), load);
}
