/*-------------------------------------------------------------------------
 *
 * jsquery.h
 *     Definitions of jsquery datatype
 *
 * Copyright (c) 2014, PostgreSQL Global Development Group
 * Author: Teodor Sigaev <teodor@sigaev.ru>
 *
 * IDENTIFICATION
 *    contrib/jsquery/jsquery.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef __JSQUERY_H__
#define __JSQUERY_H__

#include "access/gin.h"
#include "fmgr.h"
#include "utils/numeric.h"
#include "utils/jsonb.h"

typedef struct
{
	int32	vl_len_;	/* varlena header (do not touch directly!) */
} JsQuery;

#define DatumGetJsQueryP(d)	((JsQuery*)DatumGetPointer(PG_DETOAST_DATUM(d)))
#define PG_GETARG_JSQUERY(x)	DatumGetJsQueryP(PG_GETARG_DATUM(x))
#define PG_RETURN_JSQUERY(p)	PG_RETURN_POINTER(p)

typedef enum JsQueryItemType {
		jqiNull = jbvNull, 
		jqiString = jbvString, 
		jqiNumeric = jbvNumeric, 
		jqiBool = jbvBool, 
		jqiArray = jbvArray, 
		jqiAnd = '&', 
		jqiOr = '|', 
		jqiNot = '!',
		jqiEqual = '=',
		jqiLess  = '<',
		jqiGreater  = '>',
		jqiLessOrEqual = '{',
		jqiGreaterOrEqual = '}',
		jqiContains = '@',
		jqiContained = '^',
		jqiOverlap = 'O',
		jqiAny = '*',
		jqiAnyArray = '#',
		jqiAnyKey = '%',
		jqiKey = 'K',
		jqiCurrent = '$',
		jqiIn = 'I'
} JsQueryItemType;

typedef struct JsQueryItemR {
	JsQueryItemType	type;
	int32			nextPos;
	char			*base;

	union {
		struct {
			char		*data;
			int			datalen; /* filled only for string */
		} value; 

		struct {
			int32	left;
			int32	right;
		} args;
		int32		arg;
		struct {
			int		nelems;
			int		current;
			int32	*arrayPtr;
		} array;
	};


} JsQueryItemR;

extern void jsqInit(JsQueryItemR *v, JsQuery *js);
extern void jsqInitByBuffer(JsQueryItemR *v, char *base, int32 pos);
extern bool jsqGetNext(JsQueryItemR *v, JsQueryItemR *a);
extern void jsqGetArg(JsQueryItemR *v, JsQueryItemR *a);
extern void jsqGetLeftArg(JsQueryItemR *v, JsQueryItemR *a);
extern void jsqGetRightArg(JsQueryItemR *v, JsQueryItemR *a);
extern Numeric	jsqGetNumeric(JsQueryItemR *v);
extern bool		jsqGetBool(JsQueryItemR *v);
extern char * jsqGetString(JsQueryItemR *v, int32 *len);
extern void jsqIterateInit(JsQueryItemR *v);
extern bool jsqIterateArray(JsQueryItemR *v, JsQueryItemR *e);

/*
 * Parsing
 */

typedef struct JsQueryItem JsQueryItem;

struct JsQueryItem {
	JsQueryItemType	type;
	JsQueryItem	*next; /* next in path */

	union {
		struct {
			JsQueryItem	*left;
			JsQueryItem	*right;
		} args;

		JsQueryItem	*arg;

		Numeric		numeric;
		bool		boolean;
		struct {
			uint32      len;
			char        *val; /* could not be not null-terminated */
		} string;

		struct {
			int			nelems;
			JsQueryItem	**elems;
		} array;
	};
};

extern JsQueryItem* parsejsquery(const char *str, int len);

void alignStringInfoInt(StringInfo buf);

/* jsquery_extract.c */

typedef enum
{
	iAny 		= jqiAny,
	iAnyArray 	= jqiAnyArray,
	iKey 		= jqiKey,
	iAnyKey 	= jqiAnyKey
} PathItemType;

typedef struct PathItem PathItem;
struct PathItem
{
	PathItemType	type;
	int				len;
	char		   *s;
	PathItem	   *parent;
};

typedef enum
{
	eScalar = 1,
	eAnd	= jqiAnd,
	eOr		= jqiOr,
	eNot 	= jqiNot
} ExtractedNodeType;

typedef struct ExtractedNode ExtractedNode;
struct ExtractedNode
{
	ExtractedNodeType	type;
	PathItem		   *path;
	bool				indirect;
	union
	{
		struct
		{
			ExtractedNode **items;
			int				count;
		} args;
		struct
		{
			bool			inequality;
			bool			leftInclusive;
			bool			rightInclusive;
			JsQueryItemR	*exact;
			JsQueryItemR	*leftBound;
			JsQueryItemR	*rightBound;
		} bounds;
		int	entryNum;
	};
};

typedef int (*MakeEntryHandler)(ExtractedNode *node, Pointer extra);

ExtractedNode *extractJsQuery(JsQuery *jq, MakeEntryHandler handler, Pointer extra);
bool execRecursive(ExtractedNode *node, bool *check);
bool execRecursiveTristate(ExtractedNode *node, GinTernaryValue *check);

#endif
