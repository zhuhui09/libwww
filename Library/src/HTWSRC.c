/*			Parse WAIS Source file			HTWSRC.c
**			======================
**
**	This module parses a stream with WAIS source file
**	format information on it and creates a structured stream.
**	That structured stream is then converted into whatever.
**
*/

#include "HTWSRC.h"


#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include "HTML.h"

#include "HTUtils.h"
#include "tcp.h"

#define BIG 10000		/* Arbitrary limit to value length */
#define PARAM_MAX BIG
#define CACHE_PERIOD (7*86400)	/* Time to keep .src file in seconds */

#define HEX_ESCAPE '%'

struct _HTStructured {
	CONST HTStructuredClass *	isa;
	/* ... */
};

#define PUTC(c) (*me->targetClass.put_character)(me->target, c)
#define PUTS(s) (*me->targetClass.put_string)(me->target, s)
#define START(e) (*me->targetClass.start_element)(me->target, e, 0, 0)
#define END(e) (*me->targetClass.end_element)(me->target, e)


/*	Here are the parameters which can be specified in a  source file
*/
PRIVATE CONST char* par_name[] = {
	"version", 
	"ip-address",
#define PAR_IP_NAME 2
	"ip-name", 
#define PAR_TCP_PORT 3
	"tcp-port", 
#define PAR_DATABASE_NAME 4
	"database-name",
#define PAR_COST 5
	"cost", 
#define PAR_COST_UNIT 6
	"cost-unit", 
#define PAR_FREE 7
	"free",	
#define PAR_MAINTAINER 8
	"maintainer", 	
#define PAR_DESCRIPTION 9
	"description", 	
	"source",
	0,				/* Terminate list */
#define PAR_COUNT 11
} ;


enum tokenstate { beginning, before_tag, colon, before_value,
		value, quoted_value, done };


/*		Stream Object
**		------------
**
**	The target is the structured stream down which the
**	parsed results will go.
**
**	all the static stuff below should go in here to make it reentrant
*/

struct _HTStream {
	CONST HTStreamClass *	isa;
	HTStructured *		target;
	HTStructuredClass	targetClass;
	char *			par_value[PAR_COUNT];
	enum tokenstate 	state;
	char 			param[BIG+1];
	int			param_number;
	int			param_count;
};




PUBLIC CONST char * hex = "0123456789ABCDEF";

/*	Decode one hex character
*/

PUBLIC char from_hex ARGS1(char, c)
{
    return 		  (c>='0')&&(c<='9') ? c-'0'
			: (c>='A')&&(c<='F') ? c-'A'+10
			: (c>='a')&&(c<='f') ? c-'a'+10
			:		       0;
}

/*	Turn database name into a path element
**	--------------------------------------
**
**	Creates new string which must be freed.
*/
PUBLIC char * HTDeSlash ARGS1(CONST char *, db)
{
#define ACCEPTABLE(c)	((c!='/')&&(c!='#')&&(c!='%')&&(c>' ')&&(c<='z'))
    CONST char * p;
    char * q;
    char * result;
    int unacceptable = 0;
    for(p=db; *p; p++)
        if (!ACCEPTABLE(*p))
		unacceptable++;
    result = (char *) malloc(p-db + unacceptable+ unacceptable + 1);
    for(q=result, p=db; *p; p++)
	if (!ACCEPTABLE(*p)) {
	    *q++ = HEX_ESCAPE;	/* Means hex commming */
	    *q++ = hex[(*p) >> 4];
	    *q++ = hex[(*p) & 15];
	}
	else *q++ = *p;
    *q++ = 0;			/* Terminate */
    return result;
}

/*	Reverse Operation
**	-----------------
*/
/*		Return value must be freed
*/
PUBLIC char * HTEnSlash ARGS1(CONST char *, s)
{
    CONST char *p;
    char *z;
    char * result = (char *)malloc(strlen(s)+1);
    for(z=result, p=s; *p ;) {
	if (*p == HEX_ESCAPE) {
	    char c;
	    unsigned int b;
	    p++;
	    c = *p++;
	    b =   from_hex(c);
	    c = *p++;
	    if (!c) break;	/* Odd number of chars! */
	    *z++ = (b<<4) + from_hex(c);
	} else {
	    *z++ = *p++;	/* Non-hex */
	}
    }
    *z = 0;			/* Terminate */
    return result;
}


/*			State machine
**			-------------
**
** On entry,
**	me->state	is a valid state (see WSRC_init)
**	c		is the next character
** On exit,
** 	returns	1	Done with file
**		0	Continue. me->state is updated if necessary.
**		-1	Syntax error error
*/


/*		Treat One Character
**		-------------------
*/
PRIVATE void WSRCParser_put_character ARGS2(HTStream*, me, char, c)
{
    switch (me->state) {
    case beginning:
        if (c=='(') me->state = before_tag;
	break;
	
    case before_tag:
        if (c==')') {
	    me->state = done;
	    return;			/* Done with input file */
	} else if (c==':') {
	    me->param_count = 0;
	    me->state = colon;
	}				/* Ignore other text */
	break;

    case colon:
        if (WHITE(c)) {
	    me->param[me->param_count++] = 0;	/* Terminate */
	    for(me->param_number = 0; par_name[me->param_number]; me->param_number++) {
		if (0==strcmp(par_name[me->param_number], me->param)) {
		    break;
		}
	    }
	    if (!par_name[me->param_number]) {	/* Unknown field */
	        if (TRACE) fprintf(stderr,
		    "WAISGate: Unknown field `%s' in source file\n",
		    me->param);
		me->state = before_tag;	/* Could be better ignore */
		return;
	    }
	    me->state = before_value;
	} else {
	    if (me->param_count < PARAM_MAX)  me->param[me->param_count++] = c;
	}
	break;
	
    case before_value:
        if (c==')') {
	    me->state = done;
	    return;			/* Done with input file */
	}
	if (WHITE(c)) return;		/* Skip white space */
	me->param_count = 0;
	if (c=='"') {
	    me->state = quoted_value;
	    break;
	}
	me->state = (c=='"') ? quoted_value : value;
	me->param[me->param_count++] = c;	/* Don't miss first character */
	break;

    case value:
        if (WHITE(c)) {
	    me->param[me->param_count] = 0;
	    StrAllocCopy(me->par_value[me->param_number], me->param);
	    me->state = before_tag;
	} else {
	    if (me->param_count < PARAM_MAX)  me->param[me->param_count++] = c;
	}
	break;

    case quoted_value:
        if (c=='"') {
	    me->param[me->param_count] = 0;
	    StrAllocCopy(me->par_value[me->param_number], me->param);
	    me->state = before_tag;
	} else {
	    if (me->param_count < PARAM_MAX)  me->param[me->param_count++] = c;
	}
	break;
	
    case done:				/* Ignore anything after EOF */
	return;

    } /* switch me->state */
}


/*			Output equivalent HTML
**			----------------------
**
*/

void give_parameter ARGS2(HTStream *, me, int, p)
{
    PUTS(par_name[p]);
    if (me->par_value[p]) {
	PUTS(": ");
	PUTS(me->par_value[p]);
	PUTS("; ");
    } else {
        PUTS(" NOT GIVEN in source file; ");
    }
}


/*			Generate Outout
**			===============
*/
void WSRC_gen_html ARGS1(HTStream *, me)

{
    if (me->par_value[PAR_DATABASE_NAME]) {
	char * shortname = 0;
	int l;
	StrAllocCopy(shortname, me->par_value[PAR_DATABASE_NAME]);
	l = strlen(shortname);
	if ( l > 4 && !strcasecomp(shortname + l -4, ".src")) {
	    shortname[l-4] = 0;	/* Chop of .src -- boring! */
	}
	
	START(HTML_TITLE);
	PUTS(shortname);
	PUTS(" index");
	END(HTML_TITLE);
    
	START(HTML_H1);
	PUTS(shortname);
	PUTS(" index");
	END(HTML_H1);
    }
    
    START(HTML_DL);		/* Definition list of details */
    
    START(HTML_DT);
    PUTS("Access links");
    START(HTML_DD);
    if (me->par_value[PAR_IP_NAME] &&
	me->par_value[PAR_DATABASE_NAME]) {

	char WSRC_address[256];
	char * www_database;
	www_database = HTDeSlash(me->par_value[PAR_DATABASE_NAME]);
	sprintf(WSRC_address, "wais://%s:%s/%s",
	    me->par_value[PAR_IP_NAME],
	    me->par_value[PAR_TCP_PORT] ? me->par_value[PAR_TCP_PORT] : "210",
	    www_database);
    
	HTStartAnchor(me->target, NULL, WSRC_address);
	PUTS("Direct access");
	END(HTML_A);
	
	PUTS(" or ");
	
	sprintf(WSRC_address, "http://info.cern.ch:8001/%s:%s/%s",
	    me->par_value[PAR_IP_NAME],
	    me->par_value[PAR_TCP_PORT] ? me->par_value[PAR_TCP_PORT] : "210",
	    www_database);
	HTStartAnchor(me->target, NULL, WSRC_address);
	PUTS("Through CERN gateway");
	END(HTML_A);
	
	free(www_database);
	
    } else {
        give_parameter(me, PAR_IP_NAME);
        give_parameter(me, PAR_IP_NAME);
    }

    if (me->par_value[PAR_MAINTAINER]) {
	START(HTML_DT);
	PUTS("Maintainer");
	START(HTML_DD);
	PUTS(me->par_value[PAR_MAINTAINER]);
    }
    START(HTML_DT);
    PUTS("Host");
    START(HTML_DD);
    PUTS(me->par_value[PAR_IP_NAME]);

    END(HTML_DL);

    START(HTML_PRE);		/* Preformatted description */
    PUTS(me->par_value[PAR_DESCRIPTION]);
    END(HTML_PRE);

    (*me->targetClass.end_document)(me->target);
    (*me->targetClass.free)(me->target);
    
    return;
} /* generate html */


PRIVATE void WSRCParser_put_string ARGS2(HTStream *, context, CONST char*, str)
{
    CONST char *p;
    for(p=str; *p; p++)
        WSRCParser_put_character(context, *p);
}


PRIVATE void WSRCParser_write ARGS3(
		HTStream *, 	context,
		CONST char*, 	str,
		int, 		l)
{
    CONST char *p;
    CONST char *e = str+l;
    for(p=str; p<e; p++)
        WSRCParser_put_character(context, *p);
}


PRIVATE void WSRCParser_free ARGS1(HTStream *, me)
{
    WSRC_gen_html(me);
    {
	int p;
	for(p=0; par_name[p]; p++) {	/* Clear out old values */
	    if (me->par_value[p]) {
		free(me->par_value[p]);
	    }
	}
    }
    free(me);
}

PRIVATE void WSRCParser_end_document ARGS1(HTStream *, me)
{
/* Nothing */
}


/*		Stream subclass		-- method routines
**		---------------
*/

HTStreamClass WSRCParserClass = {
	"WSRCParser",
	WSRCParser_free,
	WSRCParser_end_document,
	WSRCParser_put_character,
 	WSRCParser_put_string,
	WSRCParser_write

};

/*		Converter from WAIS Source to whatever
**		--------------------------------------
*/
PUBLIC HTStream* HTWSRCConvert ARGS3(
	HTPresentation *,	pres,
	HTParentAnchor *,	anchor,	
	HTStream *,		sink)
{
    HTStream * me = (HTStream*) malloc(sizeof(*me));
    if (!me) outofmem(__FILE__, "HTWSRCConvert");

    me->isa = &WSRCParserClass;
    me->target = HTML_new(anchor, pres->rep_out, sink);
    me->targetClass = *me->target->isa;

    {
	int p;
	for(p=0; p < PAR_COUNT; p++) {	/* Clear out parameter values */
	    me->par_value[p] = 0;
	}
    }
    me->state = beginning;

    return me;
}



/*			Main Program    -- NOT USED -- HISTORICAL
**			============
**
** Takes .src on stdin and makes cache file.
*/

#ifdef TEST
PUBLIC int WWW_TraceFlag = 0;
int main(int argc, char *argv[])
{
    char c;
    
    if (argc>1) 
        WWW_TraceFlag = 0==strcmp(argv[1], "-v") ? 1 : 0;
    
    for(WSRC_init(); (c=getc(stdin)) != EOF; ) {
	if (WSRC_treat(c)) break;
        /* fprintf(stderr, "char '%c', state = %d\n", c, me->state); */
    }
    WSRC_gen_html();
        
    return 0;
    
} /* main */
#endif

