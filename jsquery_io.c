/*-------------------------------------------------------------------------
 *
 * jsquery_io.c
 *     I/O functions for jsquery datatype
 *
 * Copyright (c) 2014, PostgreSQL Global Development Group
 * Author: Teodor Sigaev <teodor@sigaev.ru>
 *
 * IDENTIFICATION
 *    contrib/jsquery/jsquery_io.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/json.h"

#include "jsquery.h"

PG_MODULE_MAGIC;

void
alignStringInfoInt(StringInfo buf)
{
	switch(INTALIGN(buf->len) - buf->len)
	{
		case 3:
			appendStringInfoCharMacro(buf, 0);
		case 2:
			appendStringInfoCharMacro(buf, 0);
		case 1:
			appendStringInfoCharMacro(buf, 0);
		default:
			break;
	}
}

static int
flattenJsQueryItem(StringInfo buf, JsQueryItem *item)
{
	int32	pos = buf->len - VARHDRSZ; /* position from begining of jsquery data */
	int32	chld, next;

	check_stack_depth();

	appendStringInfoChar(buf, (char)item->type);
	alignStringInfoInt(buf);

	next = (item->next) ? buf->len : 0;
	appendBinaryStringInfo(buf, (char*)&next /* fake value */, sizeof(next));

	switch(item->type)
	{
		case jqiKey:
		case jqiString:
			appendBinaryStringInfo(buf, (char*)&item->string.len, sizeof(item->string.len));
			appendBinaryStringInfo(buf, item->string.val, item->string.len);
			appendStringInfoChar(buf, '\0');
			break;
		case jqiNumeric:
			appendBinaryStringInfo(buf, (char*)item->numeric, VARSIZE(item->numeric));
			break;
		case jqiBool:
			appendBinaryStringInfo(buf, (char*)&item->boolean, sizeof(item->boolean));
			break;
		case jqiArray:
			{
				int32	i, arrayStart;

				appendBinaryStringInfo(buf, (char*)&item->array.nelems, sizeof(item->array.nelems));
				arrayStart = buf->len;

				/* reserve place for "pointers" to array's elements */
				for(i=0; i<item->array.nelems; i++)
					appendBinaryStringInfo(buf, (char*)&i /* fake value */, sizeof(i));

				for(i=0; i<item->array.nelems; i++)
				{
					chld = flattenJsQueryItem(buf, item->array.elems[i]);
					*(int32*)(buf->data + arrayStart + i * sizeof(i)) = chld;
				}

			}
			break;
		case jqiAnd:
		case jqiOr:
			{
				int32	left, right;

				left = buf->len;
				appendBinaryStringInfo(buf, (char*)&left /* fake value */, sizeof(left));
				right = buf->len;
				appendBinaryStringInfo(buf, (char*)&right /* fake value */, sizeof(right));

				chld = flattenJsQueryItem(buf, item->args.left);
				*(int32*)(buf->data + left) = chld;
				chld = flattenJsQueryItem(buf, item->args.right);
				*(int32*)(buf->data + right) = chld;
			}
			break;
		case jqiEqual:
		case jqiIn:
		case jqiLess:
		case jqiGreater:
		case jqiLessOrEqual:
		case jqiGreaterOrEqual:
		case jqiContains:
		case jqiContained:
		case jqiOverlap:
		case jqiNot:
			{
				int32 arg;

				arg = buf->len;
				appendBinaryStringInfo(buf, (char*)&arg /* fake value */, sizeof(arg));

				chld = flattenJsQueryItem(buf, item->arg);
				*(int32*)(buf->data + arg) = chld;
			}
			break;
		case jqiNull:
		case jqiCurrent:
		case jqiAny:
		case jqiAnyArray:
		case jqiAnyKey:
			break;
		default:
			elog(ERROR, "Unknown JsQueryItem type: %d", item->type);
	}

	if (item->next)
		*(int32*)(buf->data + next) = flattenJsQueryItem(buf, item->next);

	return  pos;
}

PG_FUNCTION_INFO_V1(jsquery_in);
Datum
jsquery_in(PG_FUNCTION_ARGS)
{
	char			*in = PG_GETARG_CSTRING(0);
	int32			len = strlen(in);
	JsQueryItem		*jsquery = parsejsquery(in, len);
	JsQuery			*res;
	StringInfoData	buf;

	initStringInfo(&buf);
	enlargeStringInfo(&buf, 4 * len /* estimation */); 

	appendStringInfoSpaces(&buf, VARHDRSZ);

	if (jsquery != NULL)
	{
		flattenJsQueryItem(&buf, jsquery);

		res = (JsQuery*)buf.data;
		SET_VARSIZE(res, buf.len);

		PG_RETURN_JSQUERY(res);
	}

	PG_RETURN_NULL();
}

static void
printOperation(StringInfo buf, JsQueryItemType type)
{
	switch(type)
	{
		case jqiAnd:
			appendBinaryStringInfo(buf, " & ", 3); break;
		case jqiOr:
			appendBinaryStringInfo(buf, " | ", 3); break;
		case jqiEqual:
			appendBinaryStringInfo(buf, " = ", 3); break;
		case jqiLess:
			appendBinaryStringInfo(buf, " < ", 3); break;
		case jqiGreater:
			appendBinaryStringInfo(buf, " > ", 3); break;
		case jqiLessOrEqual:
			appendBinaryStringInfo(buf, " <= ", 4); break;
		case jqiGreaterOrEqual:
			appendBinaryStringInfo(buf, " >= ", 4); break;
		case jqiContains:
			appendBinaryStringInfo(buf, " @> ", 4); break;
		case jqiContained:
			appendBinaryStringInfo(buf, " <@ ", 4); break;
		case jqiOverlap:
			appendBinaryStringInfo(buf, " && ", 4); break;
		default:
			elog(ERROR, "Unknown JsQueryItem type: %d", type);
	}
}

static void
printJsQueryItem(StringInfo buf, JsQueryItemR *v, bool inKey, bool printBracketes)
{
	JsQueryItemR	elem;
	bool			first = true;

	check_stack_depth();

	switch(v->type)
	{
		case jqiNull:
			appendStringInfoString(buf, "null");
			break;
		case jqiKey:
			if (inKey)
				appendStringInfoChar(buf, '.');
			/* follow next */
		case jqiString:
			escape_json(buf, jsqGetString(v, NULL));
			break;
		case jqiNumeric:
			appendStringInfoString(buf,
									DatumGetCString(DirectFunctionCall1(numeric_out,
										PointerGetDatum(jsqGetNumeric(v)))));
		break;
		case jqiBool:
			if (jsqGetBool(v))
				appendBinaryStringInfo(buf, "true", 4);
			else
				appendBinaryStringInfo(buf, "false", 5);
			break;
		case jqiArray:
			if (printBracketes)
				appendStringInfoChar(buf, '['); 

			while(jsqIterateArray(v, &elem))
			{
				if (first == false)
					appendBinaryStringInfo(buf, ", ", 2);
				else
					first = false;
				printJsQueryItem(buf, &elem, false, true);
			}

			if (printBracketes)
				appendStringInfoChar(buf, ']');
			break;
		case jqiAnd:
		case jqiOr:
			appendStringInfoChar(buf, '(');
			jsqGetLeftArg(v, &elem);
			printJsQueryItem(buf, &elem, false, true);
			printOperation(buf, v->type);
			jsqGetRightArg(v, &elem);
			printJsQueryItem(buf, &elem, false, true);
			appendStringInfoChar(buf, ')');
			break;
		case jqiEqual:
		case jqiLess:
		case jqiGreater:
		case jqiLessOrEqual:
		case jqiGreaterOrEqual:
		case jqiContains:
		case jqiContained:
		case jqiOverlap:
			printOperation(buf, v->type);
			jsqGetArg(v, &elem);
			printJsQueryItem(buf, &elem, false, true);
			break;
		case jqiIn:
			appendBinaryStringInfo(buf, " IN (", 5);
			jsqGetArg(v, &elem);
			printJsQueryItem(buf, &elem, false, false);
			appendStringInfoChar(buf, ')');
			break;
		case jqiNot:
			appendBinaryStringInfo(buf, "!(", 2);
			jsqGetArg(v, &elem);
			printJsQueryItem(buf, &elem, false, true);
			appendStringInfoChar(buf, ')');
			break;
		case jqiAny:
			if (inKey)
				appendStringInfoChar(buf, '.');
			appendStringInfoChar(buf, '*');
			break;
		case jqiCurrent:
			if (inKey)
				appendStringInfoChar(buf, '.');
			appendStringInfoChar(buf, '$');
			break;
		case jqiAnyArray:
			if (inKey)
				appendStringInfoChar(buf, '.');
			appendStringInfoChar(buf, '#');
			break;
		case jqiAnyKey:
			if (inKey)
				appendStringInfoChar(buf, '.');
			appendStringInfoChar(buf, '%');
			break;
		default:
			elog(ERROR, "Unknown JsQueryItem type: %d", v->type);
	}

	if (jsqGetNext(v, &elem))
		printJsQueryItem(buf, &elem, true, true);
}

PG_FUNCTION_INFO_V1(jsquery_out);
Datum
jsquery_out(PG_FUNCTION_ARGS)
{
	JsQuery			*in = PG_GETARG_JSQUERY(0);
	StringInfoData	buf;
	JsQueryItemR	v;

	initStringInfo(&buf);
	enlargeStringInfo(&buf, VARSIZE(in) /* estimation */); 

	jsqInit(&v, in);
	printJsQueryItem(&buf, &v, false, true);

	PG_RETURN_CSTRING(buf.data);
}


