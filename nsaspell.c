/*
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1(the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/.
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis,WITHOUT WARRANTY OF ANY KIND,either express or implied. See
 * the License for the specific language governing rights and limitations
 * under the License.
 *
 * Alternatively,the contents of this file may be used under the terms
 * of the GNU General Public License(the "GPL"),in which case the
 * provisions of GPL are applicable instead of those above.  If you wish
 * to allow use of your version of this file only under the terms of the
 * GPL and not to allow others to use your version of this file under the
 * License,indicate your decision by deleting the provisions above and
 * replace them with the notice and other provisions required by the GPL.
 * If you do not delete the provisions above,a recipient may use your
 * version of this file under either the License or the GPL.
 *
 *
 * Author Vlad Seryakov vlad@crystalballinc.com
 * 
 */

/*
 * nsaspell.c -- Interface to aspell library
 *
 */

#include "ns.h"
#include "aspell.h"

typedef struct _AspellSession {
  struct _AspellSession *next,*prev;
  unsigned long id;
  time_t access_time;
  AspellConfig *config;
  AspellSpeller *speller;
  AspellDocumentChecker *checker;
} AspellSession;

static int AspellInterpInit(Tcl_Interp *interp,void *context);
static int AspellCmd(void *context,Tcl_Interp *interp,int objc,Tcl_Obj *CONST objv[]);
static int AspellList(Tcl_Interp *interp,AspellSpeller *speller,const AspellWordList *wl);
static int AspellCheckText(Tcl_Interp *interp,AspellSession *asp,char *text,int textlen,int suggest);

static Ns_Mutex aspellMutex;
static unsigned int aspellID = 0;
static AspellSession *aspellList = 0;

NS_EXPORT int Ns_ModuleVersion = 1;

NS_EXPORT int
Ns_ModuleInit(char *server, char *module)
{
    char *path;

    path = Ns_ConfigGetPath(server,module,NULL);

    return Ns_TclInitInterps(server,AspellInterpInit,0);
}

static int
AspellInterpInit(Tcl_Interp *interp, void *context)
{
    Tcl_CreateObjCommand(interp,"ns_aspell",AspellCmd,context,0);
    return NS_OK;
}

int
Nsaspell_Init(Tcl_Interp *interp)
{
    Tcl_CreateObjCommand(interp,"ns_aspell",AspellCmd,0,0);
    return 0;
}

static int
AspellCmd(void *context,Tcl_Interp *interp,int objc,Tcl_Obj * CONST objv[])
{
    int i,cmd;
    AspellSession *asp;
    AspellConfig *config;
    AspellCanHaveError *ret;

    enum commands {
        cmdSessions,
        cmdCreate,
        cmdDestroy,
        cmdPersonalWordList,
        cmdSessionWordList,
        cmdMainWordList,
        cmdSetConfig,
        cmdGetConfig,
        cmdGetConfigList,
        cmdClearSession,
        cmdSave,
        cmdCheckWord,
        cmdSuggestWord,
        cmdPrintConfig,
        cmdPersonalAdd,
        cmdSessionAdd,
        cmdDictList,
        cmdCheckText,
        cmdSuggestText
    };
      
    static const char *sCmd[] = {
        "sessions",
        "create",
        "destroy",
        "personalwordlist",
        "sessionwordlist",
        "mainwordlist",
        "setconfig",
        "getconfig",
        "getconfiglist",
        "clearsession",
        "save",
        "checkword",
        "suggestword",
        "printconfig",
        "personaladd",
        "sessionadd",
        "dictlist",
        "checktext",
        "suggesttext",
        0
    };

    if(objc < 2) {
      Tcl_AppendResult(interp, "wrong # args: should be ns_aspell command ?args ...?",0);
      return TCL_ERROR;
    }
    if(Tcl_GetIndexFromObj(interp,objv[1],sCmd,"command",TCL_EXACT,(int *)&cmd) != TCL_OK)
      return TCL_ERROR;

    if(cmd > cmdCreate) {
      if(objc < 3) {
        Tcl_WrongNumArgs(interp,2,objv,"#session ...");
        return TCL_ERROR;
      }
      i = atoi(Tcl_GetString(objv[2]));
      Ns_MutexLock(&aspellMutex);
      for(asp = aspellList;asp;asp = asp->next) if(asp->id == i) break;
      Ns_MutexUnlock(&aspellMutex);
      if(!asp) {
        Tcl_WrongNumArgs(interp,3,objv,":unknown session id");
        return TCL_ERROR;
      }
      asp->access_time = time(0);
    }

    switch(cmd) {
     case cmdSessions:
        Ns_MutexLock(&aspellMutex);
        for(asp = aspellList;asp;asp = asp->next) {
          char buf[64];
          sprintf(buf,"%lu %lu ",asp->id,asp->access_time);
          Tcl_AppendResult(interp,buf,0);
        }
        Ns_MutexUnlock(&aspellMutex);
        break;

     case cmdCreate:
        if(objc < 3) {
          Tcl_WrongNumArgs(interp,2,objv,"language ?-size size? ?-jargon jargon? ?-encoding encoding?");
          return TCL_ERROR;
        }
        config = new_aspell_config();
        aspell_config_replace(config,"lang",Tcl_GetString(objv[2]));
        for(i = 3; i < objc - 1; i += 2) {
          char *key = Tcl_GetString(objv[i]);
          if(*key == '-') key++;
          aspell_config_replace(config,key,Tcl_GetString(objv[i+1]));
        } 
        ret = new_aspell_speller(config);
        delete_aspell_config(config);
        if(aspell_error(ret) != 0) {
          Tcl_AppendResult(interp,aspell_error_message(ret),0);
          delete_aspell_can_have_error(ret);
          return TCL_ERROR;
        }
        asp = (AspellSession*)ns_calloc(1,sizeof(AspellSession));
        asp->speller = to_aspell_speller(ret);
        asp->config = aspell_speller_config(asp->speller);
        asp->access_time = time(0);
        ret = new_aspell_document_checker(asp->speller);
        if(aspell_error(ret)) {
          Tcl_AppendResult(interp,aspell_error_message(ret),0);
          delete_aspell_speller(asp->speller);
          ns_free(asp);
          return TCL_ERROR;
        }
        asp->checker = to_aspell_document_checker(ret);
        // Link new session to global session list
        Ns_MutexLock(&aspellMutex);
        asp->id = ++aspellID;
        asp->next = aspellList;
        if(aspellList) aspellList->prev = asp;
        aspellList = asp;
        Ns_MutexUnlock(&aspellMutex);
        Tcl_SetObjResult(interp,Tcl_NewIntObj(asp->id));
        break;

     case cmdDestroy:
        delete_aspell_document_checker(asp->checker);
        delete_aspell_speller(asp->speller);
        Ns_MutexLock(&aspellMutex);
        if(asp->prev) asp->prev->next = asp->next;
        if(asp->next) asp->next->prev = asp->prev;
        if(asp == aspellList) aspellList = asp->next;
        Ns_MutexUnlock(&aspellMutex);
        ns_free(asp);
        break;

     case cmdPersonalWordList:
        return AspellList(interp,asp->speller,aspell_speller_personal_word_list(asp->speller));

     case cmdSessionWordList:
        return AspellList(interp,asp->speller,aspell_speller_session_word_list(asp->speller));

     case cmdMainWordList:
        return AspellList(interp,asp->speller,aspell_speller_main_word_list(asp->speller));

     case cmdSetConfig:
        if(objc < 5) {
          Tcl_WrongNumArgs(interp,3,objv,"name value");
          return TCL_ERROR;
        }
        aspell_config_replace(asp->config,Tcl_GetString(objv[3]),Tcl_GetString(objv[4]));
        if(aspell_config_error(asp->config)) {
          Tcl_AppendResult(interp,aspell_config_error_message(asp->config),0);
          return TCL_ERROR;
        }
        break;

     case cmdGetConfig:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"name");
          return TCL_ERROR;
        }
        Tcl_AppendResult(interp,aspell_config_retrieve(asp->config,Tcl_GetString(objv[3])),0);
        break;

     case cmdGetConfigList:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"name");
          return TCL_ERROR;
        }
	const char * val;
	AspellStringEnumeration *els;
        AspellStringList *lst = new_aspell_string_list();
	AspellMutableContainer *lst0 = aspell_string_list_to_mutable_container(lst);
	aspell_config_retrieve_list(asp->config,Tcl_GetString(objv[3]),lst0);
	if(aspell_config_error(asp->config)) {
          Tcl_AppendResult(interp,aspell_config_error_message(asp->config),0);
          return TCL_ERROR;
        }
	els = aspell_string_list_elements(lst);
	while((val = aspell_string_enumeration_next(els))) Tcl_AppendResult(interp,val," ",0);
	delete_aspell_string_enumeration(els);
	delete_aspell_string_list(lst);
        break;

     case cmdClearSession:
        aspell_speller_clear_session(asp->speller);
        if(aspell_speller_error(asp->speller)) {
          Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
          return TCL_ERROR;
        }
        break;

     case cmdSave:
        aspell_speller_save_all_word_lists(asp->speller);
        if(aspell_speller_error(asp->speller)) {
          Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
          return TCL_ERROR;
        }
        break;

     case cmdCheckWord:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"word");
          return TCL_ERROR;
        }
        switch(aspell_speller_check(asp->speller,Tcl_GetString(objv[3]),-1)) {
         case 0:
           Tcl_AppendResult(interp,"0",0);
           break;
         case 1:
           Tcl_AppendResult(interp,"1",0);
           break;
         default:
           Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
           return TCL_ERROR;
        }
        break;

     case cmdSuggestWord:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"word");
          return TCL_ERROR;
        }
        return AspellList(interp,asp->speller,aspell_speller_suggest(asp->speller,Tcl_GetString(objv[3]),-1));

     case cmdPrintConfig: {
        const AspellKeyInfo *entry;
        AspellKeyInfoEnumeration *key_list = aspell_config_possible_elements(asp->config,0);
        while((entry = aspell_key_info_enumeration_next(key_list)))
          Tcl_AppendResult(interp,entry->name," {",aspell_config_retrieve(asp->config,entry->name),"} ",0);
        delete_aspell_key_info_enumeration(key_list);
        break;
     }

     case cmdPersonalAdd:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"word");
          return TCL_ERROR;
        }
        aspell_speller_add_to_personal(asp->speller,Tcl_GetString(objv[3]),-1);
        if(aspell_speller_error(asp->speller)) {
          Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
          return TCL_ERROR;
        }
        break;

     case cmdSessionAdd:
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"word");
          return TCL_ERROR;
        }
        aspell_speller_add_to_session(asp->speller,Tcl_GetString(objv[3]),-1);
        if(aspell_speller_error(asp->speller)) {
          Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
          return TCL_ERROR;
        }
        break;

     case cmdDictList: {
        AspellDictInfoList *dlist;
        const AspellDictInfo *entry;
        AspellDictInfoEnumeration *dels;

        dlist = get_aspell_dict_info_list(asp->config);
        dels = aspell_dict_info_list_elements(dlist);
        while((entry = aspell_dict_info_enumeration_next(dels))) {
          Tcl_AppendResult(interp,"{ {",entry->name,"} {",entry->code,"} {",entry->jargon,"} {",entry->size_str,"} {",entry->module?entry->module->name:"","} } ",0);
        }
        delete_aspell_dict_info_enumeration(dels);
        break;
     }

     case cmdCheckText: {
        char *text;
        int textlen;
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"text");
          return TCL_ERROR;
        }
        text = Tcl_GetStringFromObj(objv[3],&textlen);
        return AspellCheckText(interp,asp,text,textlen,0);
     }

     case cmdSuggestText: {
        char *text;
        int textlen;
        if(objc < 4) {
          Tcl_WrongNumArgs(interp,3,objv,"text");
          return TCL_ERROR;
        }
        text = Tcl_GetStringFromObj(objv[3],&textlen);
        return AspellCheckText(interp,asp,text,textlen,1);
     }
    }
    return TCL_OK;
}

static int
AspellList(Tcl_Interp *interp,AspellSpeller *speller,const AspellWordList *wl)
{
    if(aspell_word_list_empty(wl)) return TCL_OK;
    const char * word;
    AspellStringEnumeration *els = aspell_word_list_elements(wl);
    while((word = aspell_string_enumeration_next(els))) Tcl_AppendResult(interp,word," ",0);
    return TCL_OK;
}

static unsigned
AspellOffset(char *str,unsigned len,char *encoding)
{
    if(!encoding) return len;
    if(!strcmp(encoding,"utf-8")) {
      unsigned i,size = 0;
      for(i = 0;i < len;i++) if((str[i] & 0x80) == 0 || (str[i] & 0xC0) == 0xC0) ++size;
      return size;
    }
    if(!strcmp(encoding,"ucs-2")) return len/2;
    if(!strcmp(encoding,"ucs-4")) return len/4;
    return len;
}

static int
AspellCheckText(Tcl_Interp *interp,AspellSession *asp,char *text,int textlen,int suggest)
{
    char ws,*s,*word;
    AspellToken token;
    AspellWordList *wl;
    AspellStringEnumeration *els;
    Tcl_Obj *list = Tcl_NewListObj(0,0);
    char *encoding = (char*)aspell_config_retrieve(asp->config,"encoding");

    aspell_document_checker_reset(asp->checker);
    aspell_document_checker_process(asp->checker,text,textlen);
    if(aspell_speller_error(asp->speller)) {
      Tcl_AppendResult(interp,aspell_speller_error_message(asp->speller),0);
      return TCL_ERROR;
    }
    if(aspell_document_checker_error(asp->checker)) {
      Tcl_AppendResult(interp,aspell_document_checker_error_message(asp->checker),0);
      return TCL_ERROR;
    }
    while(token = aspell_document_checker_next_misspelling(asp->checker),token.len != 0) {
      word = text + token.offset;
      ws = word[token.len];
      word[token.len] = 0;
      Tcl_ListObjAppendElement(interp,list,Tcl_NewStringObj(word,token.len));
      Tcl_ListObjAppendElement(interp,list,Tcl_NewIntObj(AspellOffset(text,token.offset,encoding)));
      if(suggest) {
        Tcl_Obj *list2 = Tcl_NewListObj(0,0);
        wl = (AspellWordList*)aspell_speller_suggest(asp->speller,word,-1);
        if(!aspell_word_list_empty(wl)) {
          els = aspell_word_list_elements(wl);
          while((s = (char*)aspell_string_enumeration_next(els)))
            Tcl_ListObjAppendElement(interp,list2,Tcl_NewStringObj(s,-1));
        }
        Tcl_ListObjAppendElement(interp,list,list2);
      }
      word[token.len] = ws;
    }
    Tcl_SetObjResult(interp,list);
    return TCL_OK;
}
