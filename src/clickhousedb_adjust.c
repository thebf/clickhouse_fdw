#include "postgres.h"
#include "strings.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/heapam.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "commands/defrem.h"
#include "utils/hsearch.h"
#include "utils/syscache.h"
#include "utils/inval.h"
#include "utils/rel.h"
#include "catalog/dependency.h"

#include "clickhousedb_fdw.h"

static HTAB *custom_objects_cache = NULL;
static HTAB *custom_columns_cache = NULL;

static HTAB *
create_custom_objects_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(CustomObjectDef);

	return hash_create("clickhouse_fdw custom functions", 20, &ctl, HASH_ELEM);
}

static void
invalidate_custom_columns_cache(Datum arg, int cacheid, uint32 hashvalue)
{
	HASH_SEQ_STATUS status;
	CustomColumnInfo *entry;

	hash_seq_init(&status, custom_columns_cache);
	while ((entry = (CustomColumnInfo *) hash_seq_search(&status)) != NULL)
	{
		if (hash_search(custom_columns_cache,
						(void *) &entry->relid,
						HASH_REMOVE,
						NULL) == NULL)
			elog(ERROR, "hash table corrupted");
	}
}

static HTAB *
create_custom_columns_cache(void)
{
	HASHCTL		ctl;

	ctl.keysize = sizeof(Oid) + sizeof(int);
	ctl.entrysize = sizeof(CustomColumnInfo);

	CacheRegisterSyscacheCallback(ATTNUM,
								  invalidate_custom_columns_cache,
								  (Datum) 0);

	return hash_create("clickhouse_fdw custom functions", 20, &ctl, HASH_ELEM | HASH_BLOBS);
}

CustomObjectDef *checkForCustomFunction(Oid funcid)
{
	const char *proname;

	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (is_builtin(funcid))
		return NULL;

	entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_FIND, NULL);
	if (!entry)
	{
		HeapTuple	proctup;
		Form_pg_proc procform;

		Oid extoid = getExtensionOfObject(ProcedureRelationId, funcid);
		char *extname = get_extension_name(extoid);

		entry = hash_search(custom_objects_cache, (void *) &funcid, HASH_ENTER, NULL);
		entry->cf_type = CF_USUAL;

		if (extname && strcmp(extname, "istore") == 0)
		{
			proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
			if (!HeapTupleIsValid(proctup))
				elog(ERROR, "cache lookup failed for function %u", funcid);

			procform = (Form_pg_proc) GETSTRUCT(proctup);

			proname = NameStr(procform->proname);
			if (strcmp(proname, "sum") == 0)
			{
				entry->cf_type = CF_ISTORE_SUM;
				strcpy(entry->custom_name, "sumMap");
			}

			ReleaseSysCache(proctup);
		}
	}

	return entry;
}

CustomObjectDef *checkForCustomType(Oid typeoid)
{
	const char *proname;

	CustomObjectDef	*entry;
	if (!custom_objects_cache)
		custom_objects_cache = create_custom_objects_cache();

	if (is_builtin(typeoid))
		return NULL;

	entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_FIND, NULL);
	if (!entry)
	{
		Oid extoid = getExtensionOfObject(TypeRelationId, typeoid);
		char *extname = get_extension_name(extoid);

		entry = hash_search(custom_objects_cache, (void *) &typeoid, HASH_ENTER, NULL);
		entry->cf_type = CF_USUAL;

		if (extname && strcmp(extname, "istore") == 0)
			entry->cf_type = CF_ISTORE_TYPE; /* bigistore or istore */
	}

	return entry;
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
void
ApplyCustomTableOptions(CHFdwRelationInfo *fpinfo, Oid relid)
{
	ListCell	*lc;
	TupleDesc	tupdesc;
	int			attnum;
	Relation	rel;
	List	   *options;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		if (strcmp(def->defname, "engine") == 0)
		{
			static char *expected = "collapsingmergetree";
			char *val = defGetString(def);
			if (strncasecmp(val, expected, strlen(expected)) == 0)
			{
				char   *start = index(val, '('),
					   *end = rindex(val, ')');

				fpinfo->ch_table_engine = CH_COLLAPSING_MERGE_TREE;
				if (start == end)
				{
					strcpy(fpinfo->ch_table_sign_field, "sign");
					continue;
				}

				if (end - start > NAMEDATALEN)
					elog(ERROR, "invalid format of ClickHouse engine");

				strncpy(fpinfo->ch_table_sign_field, start + 1, end - start - 1);
				fpinfo->ch_table_sign_field[end - start] = '\0';
			}
		}
	}

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	rel = heap_open(relid, NoLock);
	tupdesc = RelationGetDescr(rel);

	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		bool				found;
		CustomObjectDef	   *cdef;
		CustomColumnInfo	entry_key,
						   *entry;
		custom_object_type	cf_type = CF_ISTORE_ARR;

		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
		entry_key.relid = relid;
		entry_key.varattno = attnum;

		entry = hash_search(custom_columns_cache,
				(void *) &entry_key.relid, HASH_ENTER, &found);
		if (found)
			continue;

		entry->relid = relid;
		entry->varattno = attnum;
		entry->table_engine = fpinfo->ch_table_engine;
		entry->coltype = CF_USUAL;
		strcpy(entry->colname, NameStr(attr->attname));
		strcpy(entry->signfield, fpinfo->ch_table_sign_field);

		/* If a column has the column_name FDW option, use that value */
		options = GetForeignColumnOptions(relid, attnum);
		foreach (lc, options)
		{
			DefElem    *def = (DefElem *) lfirst(lc);

			if (strcmp(def->defname, "column_name") == 0)
			{
				strncpy(entry->colname, defGetString(def), NAMEDATALEN);
				entry->colname[NAMEDATALEN - 1] = '\0';
			}
			else if (strcmp(def->defname, "arrays") == 0)
				cf_type = CF_ISTORE_ARR;
			else if (strcmp(def->defname, "keys") == 0)
				cf_type = CF_ISTORE_COL;
		}

		cdef = checkForCustomType(attr->atttypid);
		if (cdef && cdef->cf_type == CF_ISTORE_TYPE)
			entry->coltype = cf_type;
	}
	heap_close(rel, NoLock);
}

/* Get foreign relation options */
CustomColumnInfo *
GetCustomColumnInfo(Oid relid, uint16 varattno)
{
	CustomColumnInfo	entry_key,
					   *entry;

	entry_key.relid = relid;
	entry_key.varattno = varattno;

	if (custom_columns_cache == NULL)
		custom_columns_cache = create_custom_columns_cache();

	entry = hash_search(custom_columns_cache,
			(void *) &entry_key.relid, HASH_FIND, NULL);

	return entry;
}
