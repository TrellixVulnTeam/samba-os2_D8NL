/*
   Unix SMB/CIFS implementation.

   Swig interface to ldb.

   Copyright (C) 2007-2008 Jelmer Vernooij <jelmer@samba.org>

     ** NOTE! The following LGPL license applies to the ldb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _PYLDB_H_
#define _PYLDB_H_

#include <Python.h>

typedef struct {
	PyObject_HEAD
	struct ldb_context *ldb_ctx;
	TALLOC_CTX *mem_ctx;
} PyLdbObject;

PyAPI_DATA(PyTypeObject) PyLdb;
PyObject *PyLdb_FromLdbContext(struct ldb_context *ldb_ctx);
#define PyLdb_AsLdbContext(pyobj) ((PyLdbObject *)pyobj)->ldb_ctx
#define PyLdb_Check(ob) PyObject_TypeCheck(ob, &PyLdb)

typedef struct {
	PyObject_HEAD
	struct ldb_dn *dn;
	TALLOC_CTX *mem_ctx;
} PyLdbDnObject;

PyAPI_DATA(PyTypeObject) PyLdbDn;
PyObject *PyLdbDn_FromDn(struct ldb_dn *);
bool PyObject_AsDn(TALLOC_CTX *mem_ctx, PyObject *object, struct ldb_context *ldb_ctx, struct ldb_dn **dn);
#define PyLdbDn_AsDn(pyobj) ((PyLdbDnObject *)pyobj)->dn
#define PyLdbDn_Check(ob) PyObject_TypeCheck(ob, &PyLdbDn)

typedef struct {
	PyObject_HEAD
	struct ldb_message *msg;
	TALLOC_CTX *mem_ctx;
} PyLdbMessageObject;
PyAPI_DATA(PyTypeObject) PyLdbMessage;
PyObject *PyLdbMessage_FromMessage(struct ldb_message *message);
#define PyLdbMessage_Check(ob) PyObject_TypeCheck(ob, &PyLdbMessage)
#define PyLdbMessage_AsMessage(pyobj) ((PyLdbMessageObject *)pyobj)->msg

typedef struct {
	PyObject_HEAD
	struct ldb_module *mod;
	TALLOC_CTX *mem_ctx;
} PyLdbModuleObject;
PyAPI_DATA(PyTypeObject) PyLdbModule;
PyObject *PyLdbModule_FromModule(struct ldb_module *mod);
#define PyLdbModule_AsModule(pyobj) ((PyLdbModuleObject *)pyobj)->mod

typedef struct {
	PyObject_HEAD	
	struct ldb_message_element *el;
	TALLOC_CTX *mem_ctx;
} PyLdbMessageElementObject;
PyAPI_DATA(PyTypeObject) PyLdbMessageElement;
struct ldb_message_element *PyObject_AsMessageElement(TALLOC_CTX *mem_ctx, PyObject *obj, int flags, const char *name);
PyObject *PyLdbMessageElement_FromMessageElement(struct ldb_message_element *, TALLOC_CTX *mem_ctx);
#define PyLdbMessageElement_AsMessageElement(pyobj) ((PyLdbMessageElementObject *)pyobj)->el
#define PyLdbMessageElement_Check(ob) PyObject_TypeCheck(ob, &PyLdbMessageElement)

typedef struct {
	PyObject_HEAD
	struct ldb_parse_tree *tree;
	TALLOC_CTX *mem_ctx;
} PyLdbTreeObject;
PyAPI_DATA(PyTypeObject) PyLdbTree;
PyObject *PyLdbTree_FromTree(struct ldb_parse_tree *);
#define PyLdbTree_AsTree(pyobj) ((PyLdbTreeObject *)pyobj)->tree

void PyErr_SetLdbError(int ret, struct ldb_context *ldb_ctx);
#define PyErr_LDB_ERROR_IS_ERR_RAISE(ret,ldb) \
	if (ret != LDB_SUCCESS) { \
		PyErr_SetLdbError(ret, ldb); \
		return NULL; \
	}


#endif /* _PYLDB_H_ */
