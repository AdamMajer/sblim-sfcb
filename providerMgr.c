
/*
 * providerMgr.c
 *
 * (C) Copyright IBM Corp. 2005
 *
 * THIS FILE IS PROVIDED UNDER THE TERMS OF THE COMMON PUBLIC LICENSE
 * ("AGREEMENT"). ANY USE, REPRODUCTION OR DISTRIBUTION OF THIS FILE
 * CONSTITUTES RECIPIENTS ACCEPTANCE OF THE AGREEMENT.
 *
 * You can obtain a current copy of the Common Public License from
 * http://oss.software.ibm.com/developerworks/opensource/license-cpl.html
 *
 * Author:        Adrian Schuur <schuur@de.ibm.com>
 * Based on concepts developed by Viktor Mihajlovski <mihajlov@de.ibm.com>
 *
 * Description:
 *
 * Provider manager support.
 *
*/


#define SFCB_INCL_INDICATION_SUPPORT 1

#include <signal.h>
#include <time.h>

#include "providerMgr.h"
#include "utilft.h"
#include "msgqueue.h"
#include "constClass.h"
#include "cimXmlParser.h"
#include "support.h"
#include "native.h"
#include "trace.h"
#include "queryOperation.h"
#include "selectexp.h"


extern CMPIBroker *Broker;

extern ProviderInfo *classProvInfoPtr;
extern ProviderInfo *defaultProvInfoPtr;
extern ProviderInfo *interOpProvInfoPtr;
extern ProviderInfo *forceNoProvInfoPtr;

typedef struct mgrHandler {
   void (*handler) (int *, OperationHdr *);
} MgrHandler;

static UtilHashTable *instanceProviderHt = NULL;
static UtilHashTable *instanceProvidersHt = NULL;
static UtilHashTable *assocProvidersHt = NULL;
static UtilHashTable *assocProviderHt = NULL;
static UtilHashTable *methodProviderHt = NULL;
static UtilHashTable *indicationProviderHt = NULL;

extern unsigned long exFlags;
extern ProviderRegister *pReg;

extern void processProviderInvocationRequests(ProviderInfo *);
extern CMPIObjectPath *relocateSerializedObjectPath(void *area);
extern MsgSegment setObjectPathMsgSegment(CMPIObjectPath * op);
extern MsgSegment setInstanceMsgSegment(CMPIInstance * op);
extern MsgSegment setArgsMsgSegment(CMPIArgs * args);
extern void getSerializedObjectPath(CMPIObjectPath * op, void *area);
extern void getSerializedInstance(CMPIInstance * ci, void *area);
extern MsgSegment setConstClassMsgSegment(CMPIConstClass * cl);
extern void getSerializedConstClass(CMPIConstClass * cl, void *area);
extern void getSerializedArgs(CMPIArgs * cl, void *area);
extern CMPIConstClass *relocateSerializedConstClass(void *area);
extern CMPIInstance *relocateSerializedInstance(void *area);
extern void getSerializedInstance(CMPIInstance * ci, void *area);
extern CMPIArgs *relocateSerializedArgs(void *area);
extern MsgSegment setArgsMsgSegment(CMPIArgs * args);
extern void dump(char *msg, void *a, int l);
extern void showClHdr(void *ihdr);
extern int forkProvider(ProviderInfo * info, OperationHdr * req, char **msg);

static int startUpProvider(const char* ns, const char *name);

extern void *markHeap();
extern void releaseHeap(void *hc);
extern int spSendAck(int to);

static UtilList *_getConstClassChildren(const char *ns, const char *cn);
static CMPIConstClass *_getConstClass(const char *ns, const char *cn, CMPIStatus *st);
static UtilList *_getAssocClassNames(const char *ns);

static void notSupported(int *requestor, OperationHdr * req)
{
   fprintf(stderr,"--- MSG_X_NOT_SUPPORTED\n");
   spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_NOT_SUPPORTED, 0, NULL);
   free(req);
}


static void handleSigterm(int sig)
{
   fprintf(stderr, "%s: exiting due to signal %d\n", "provider", sig);
   exit(1);
}

static void handleSigSegv(int sig)
{
   fprintf(stderr, "()%d): exiting due to a SIGSEGV signal %d - %s(%d)\n",
           currentProc, sig, __FILE__, __LINE__);
   abort();
}



/* -------------
 * ---
 *      Instance Provider support
 * ---className
 * -------------
 */
 
ProvIds getProvIds(ProviderInfo *info)
{
   ProvIds ids;
   ids.provId=info->id;
   ids.procId=info->proc->id;
   return ids;
}

int interopClass(char *className) 
{
   if (strcasecmp(className,"cim_indicationfilter")==0 ||
       strcasecmp(className,"cim_indicationsubscription")==0) return 1;
   return 0;    
}
 
static UtilHashTable **provHt(long type, int list)
{
   switch(type) {
   case INSTANCE_PROVIDER:    
      if (list) return &instanceProvidersHt;
      return &instanceProviderHt;
   case INDICATION_PROVIDER:  return &indicationProviderHt;
   default:                   return NULL;
   } 
   return NULL;
}

int nameSpaceOk(ProviderInfo *info, char *nameSpace)
{
   char **ns;
   
   _SFCB_ENTER(TRACE_PROVIDERMGR, "nameSpaceOk");
   
   _SFCB_TRACE(1,("--- testing for %s on %s",nameSpace,info->providerName));
   
   if (info->ns) for (ns=info->ns; *ns; ns++) {
      if (strcasecmp(*ns,nameSpace)==0) _SFCB_RETURN(1);
   }   
   if (info==classProvInfoPtr) _SFCB_RETURN(1);

   if (strcasecmp(nameSpace,"root/pg_interop")==0) 
      return nameSpaceOk(info,"root/interop");
   
   _SFCB_RETURN(0);
}

static ProviderInfo *lookupProvider(long type, char *className, char *nameSpace, 
      CMPIStatus *st)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "lookupProvider");
   const char *cls = className;
   ProviderInfo *info;
   UtilHashTable **ht=provHt(type,0);
   
   if (*ht == NULL) *ht =  UtilFactory->newHashTable(61,
            UtilHashTable_charKey | UtilHashTable_ignoreKeyCase);

   info = (ProviderInfo*) (*ht)->ft->get(*ht, className);
   st->rc=0;

   if (info && nameSpaceOk(info,nameSpace)) {
      _SFCB_TRACE(1,("Provider found for %s",className));
      _SFCB_RETURN(info);
   }

   
   if (interOpProvInfoPtr != forceNoProvInfoPtr && 
      (strcasecmp(className,"cim_indicationfilter")==0 ||
       strcasecmp(className,"cim_indicationsubscription")==0)) {
      _SFCB_TRACE(1,("interopProvider for %s",cls));
      _SFCB_RETURN(interOpProvInfoPtr);
   }   
   
   while (cls != NULL) {
      info = pReg->ft->getProvider(pReg, cls, type);
      if (info && nameSpaceOk(info,nameSpace)) {
         if ((*ht)->ft->get(*ht, cls)==NULL)
            (*ht)->ft->put(*ht, cls, info);
         _SFCB_RETURN(info);
      }
      else {
         _SFCB_TRACE(1,("Getting class %s",cls));
         CMPIConstClass *cc = (CMPIConstClass *) _getConstClass(nameSpace, cls, st);
         if (cc == NULL) {
            _SFCB_TRACE(1,("Returning NULL for %s",className));
           _SFCB_RETURN(NULL);
         }
         cls = cc->ft->getCharSuperClassName(cc);
      }
   }
    
   _SFCB_TRACE(1,("Default provider for %s",className));
   _SFCB_RETURN(defaultProvInfoPtr);
}

static int addProviders(long type, char *className, char *nameSpace, 
            UtilList * providerList)
{
   char *child;          
   ProviderInfo *ip;
   UtilList *children;
   int rc = CMPI_RC_OK;
   CMPIStatus st;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "addProviders");    

   ip = lookupProvider(type,className,nameSpace,&st);
   if (ip == NULL) _SFCB_RETURN(st.rc);
   
   if (ip->providerName && !providerList->ft->contains(providerList, ip)) {
      _SFCB_TRACE(1,("--- adding className: %s provider: %s",className,ip->providerName));
      providerList->ft->add(providerList, ip);
   } 
      
   _SFCB_TRACE(1,("--- getting children")); 
   children = _getConstClassChildren(nameSpace, className);
   _SFCB_TRACE(1,("--- children %p",children));
   
   if (children) {
      for (child = children->ft->getFirst(children); child;
           child = children->ft->getNext(children)) {
         _SFCB_TRACE(1,("--- add child %s",child));
         rc = addProviders(type,child, nameSpace, providerList);
          _SFCB_TRACE(1,("--- add child %s rc: %d",child,rc));
        if (rc) _SFCB_RETURN(rc);
      }
   }

   _SFCB_RETURN(rc);
}

static UtilList *lookupProviders(long type, char *className, char *nameSpace,
         CMPIStatus *st)
{
   UtilList *lst;
   UtilHashTable **ht=provHt(type,1);
   char *id;
   int rc;
   
   _SFCB_ENTER(TRACE_PROVIDERMGR, "lookupProviders");
   
   id=(char*)malloc(strlen(nameSpace)+strlen(className)+8);
   strcpy(id,nameSpace);
   strcat(id,"|");
   strcat(id,className);
   
   if (*ht == NULL) *ht =  UtilFactory->newHashTable(61,
                  UtilHashTable_charKey | UtilHashTable_ignoreKeyCase);

   lst = (*ht)->ft->get(*ht, id);

   if (lst == NULL) {
      lst = UtilFactory->newList();
      if ((rc=addProviders(type, className, nameSpace, lst))) {
         lst->ft->release(lst);
         free(id);
         st->rc=rc;
         _SFCB_RETURN(NULL);
      }
      (*ht)->ft->put(*ht, id, lst);
   }

   free(id);
  _SFCB_RETURN(lst);
}

static void lookupProviderList(long type, int *requestor, OperationHdr * req)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "lookupProviderList");
   char *className = (char *) req->className.data;
   char *nameSpace = (char *) req->nameSpace.data;
   UtilList *providers;
   unsigned long count,n;
   ProviderInfo *info;
   int dmy = 0, rc,indFound=0;
   char *msg = NULL;
   CMPIStatus st;

   providers = lookupProviders(type,className,nameSpace,&st);

   if (providers) {
      count = (n=providers->ft->size(providers)) - 1;
      _SFCB_TRACE(1,("--- found %d providers",count));

      for (info = providers->ft->getFirst(providers); info;
           info = providers->ft->getNext(providers), n--) {
         if (info->type!=FORCE_PROVIDER_NOTFOUND &&
             (rc = forkProvider(info, req, &msg)) == CMPI_RC_OK) {
            _SFCB_TRACE(1,("--- responding with  %s %p %d",info->providerName,info,count));
            spSendCtlResult(requestor, &info->providerSockets.send,
                            MSG_X_PROVIDER, count--, getProvIds(info).ids);
            if (type==INDICATION_PROVIDER) indFound++;
         }                   
         else {
            if (type==INDICATION_PROVIDER) {
               if (n>1 || indFound) continue;
            };
            spSendCtlResult(requestor, &dmy, MSG_X_PROVIDER_NOT_FOUND, count--,
                            NULL);
            if (msg) free(msg);
         }
      }
   }
   else {
      if (st.rc==CMPI_RC_ERR_INVALID_NAMESPACE) rc=MSG_X_INVALID_NAMESPACE;
      else rc=MSG_X_INVALID_CLASS;
      spSendCtlResult(requestor, &dmy, rc, 0, NULL);
   }   
   
   _SFCB_EXIT();
}

static void findProvider(long type, int *requestor, OperationHdr * req)
{
   char *className = (char *) req->className.data;
   char *nameSpace = (char *) req->nameSpace.data;
   ProviderInfo *info;
   char *msg=NULL;
   int rc;
   CMPIStatus st;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "findProvider");
   
   if ((info = lookupProvider(type,className,nameSpace,&st)) != NULL) {
      if (info->type!=FORCE_PROVIDER_NOTFOUND &&
          (rc = forkProvider(info, req, &msg)) == CMPI_RC_OK) {
         spSendCtlResult(requestor, &info->providerSockets.send, MSG_X_PROVIDER,
                         0, getProvIds(info).ids);
      }                   
      else {
         spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_PROVIDER_NOT_FOUND, 0, NULL);
         if (msg) free(msg);
      }
   }
   else {
      if (st.rc==CMPI_RC_ERR_INVALID_NAMESPACE) rc=MSG_X_INVALID_NAMESPACE;
      else rc=MSG_X_INVALID_CLASS;
      spSendCtlResult(requestor, &sfcbSockets.send, rc, 0, NULL);
   }   
   _SFCB_EXIT();
}

static void instProvider(int *requestor, OperationHdr *req)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "instProvider");
   findProvider(INSTANCE_PROVIDER, requestor, req);
   _SFCB_EXIT();
}

static void instProviderList(int *requestor, OperationHdr * req)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "instProviderList");
   lookupProviderList(INSTANCE_PROVIDER, requestor, req);
   _SFCB_EXIT();
}

#ifdef SFCB_INCL_INDICATION_SUPPORT

static void processIndProviderList(int *requestor, OperationHdr * req)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "processIndProviderList");
   lookupProviderList(INDICATION_PROVIDER, requestor, req);
   _SFCB_EXIT();
}

#endif

/* -------------
 * ---
 *      Association Provider support
 * ---
 * -------------
 */

static ProviderInfo *getAssocProvider(char *className, char *nameSpace)
{
   long type = ASSOCIATION_PROVIDER;
   const char *cls = className;
   ProviderInfo *info;
   CMPIStatus rc;

   _SFCB_ENTER(TRACE_PROVIDERASSOCMGR, "getAssocProvider");
   
   if (assocProviderHt == NULL)
      assocProviderHt = UtilFactory->newHashTable(61,
        UtilHashTable_charKey | UtilHashTable_ignoreKeyCase);

   info = (ProviderInfo *) assocProviderHt->ft->get(assocProviderHt, className);
   if (info) _SFCB_RETURN(info);

   while (cls != NULL) {
      info = pReg->ft->getProvider(pReg, cls, type);
      if (info) {
         assocProviderHt->ft->put(assocProviderHt, className, info);
         _SFCB_RETURN(info);
      }
      else {
         CMPIConstClass *cc = (CMPIConstClass *) _getConstClass(nameSpace, cls, &rc);
         if (cc == NULL) {
            _SFCB_RETURN(NULL);
         }
         cls = cc->ft->getCharSuperClassName(cc);
      }
   }
   
   if (strcasecmp(nameSpace,"interop")==0 ||
        strcasecmp(nameSpace,"pg_interop")==0) {
      _SFCB_RETURN(interOpProvInfoPtr);  
   }     

   if (interopClass(className)) _SFCB_RETURN(forceNoProvInfoPtr);
   
   _SFCB_RETURN(defaultProvInfoPtr);
}

static int addAssocProviders(char *className, char *nameSpace, UtilList * providerList)
{
   char *child;
   ProviderInfo *ip;
   UtilList *children = NULL;
   int rc = CMPI_RC_OK;

   _SFCB_ENTER(TRACE_PROVIDERASSOCMGR, "addAssocProviders");
   
   if (strcmp(className, "$ASSOCCLASSES$") != 0) {
      ip = getAssocProvider(className,nameSpace);
      if (ip == NULL) return CMPI_RC_ERR_FAILED;
     if (ip->providerName && !providerList->ft->contains(providerList, ip)) {
         _SFCB_TRACE(1, ("--- Adding %s", ip->providerName));
         providerList->ft->add(providerList, ip);
      }
      children = _getConstClassChildren(nameSpace, className);
   }
   else children = _getAssocClassNames(nameSpace);

   if (children) for (child = children->ft->getFirst(children); child;
           child = children->ft->getNext(children)) {
         rc = addAssocProviders(child, nameSpace, providerList);
         if (rc) _SFCB_RETURN(rc);
   }
   _SFCB_RETURN(rc);
}

static UtilList *getAssocProviders(char *className, char *nameSpace) 
// className maybe NULL
{
   UtilList *lst = NULL;
   _SFCB_ENTER(TRACE_PROVIDERASSOCMGR, "getAssocProviders");

   if (assocProvidersHt == NULL)
      assocProvidersHt = UtilFactory->newHashTable(61,
           UtilHashTable_charKey | UtilHashTable_ignoreKeyCase);

   if (className)
      lst = assocProvidersHt->ft->get(assocProvidersHt, className);
   if (lst == NULL) {
      lst = UtilFactory->newList();
      if (addAssocProviders(className, nameSpace, lst)) {
         lst->ft->release(lst);
         _SFCB_RETURN(NULL);
      }
      assocProvidersHt->ft->put(assocProvidersHt, className, lst);
   }
   _SFCB_RETURN(lst);
}

static void assocProviderList(int *requestor, OperationHdr * req)
{
   _SFCB_ENTER(TRACE_PROVIDERASSOCMGR, "assocProviderList");
   char *className = (char *) req->className.data;
   char *nameSpace = (char *) req->nameSpace.data;
   UtilList *providers = NULL;
   long count;
   ProviderInfo *info;
   int dmy = 0, rc;
   char *msg=NULL;

   if (className==NULL || *className == 0)
      className = "$ASSOCCLASSES$";
   
   providers = getAssocProviders(className,nameSpace);
   if (providers) {
      count = ((long) providers->ft->size(providers))-1;
      if (count >= 0) {
         for (info = providers->ft->getFirst(providers); info;
              info = providers->ft->getNext(providers)) {
            if (info->type!=FORCE_PROVIDER_NOTFOUND &&
                (rc = forkProvider(info, req, &msg)) == CMPI_RC_OK) {
               _SFCB_TRACE(1,("--- responding with  %s %p %d",info->providerName,info,count));
               spSendCtlResult(requestor, &info->providerSockets.send,
                               MSG_X_PROVIDER, count--, getProvIds(info).ids);
            }                   
            else {
               spSendCtlResult(requestor, &dmy, MSG_X_PROVIDER_NOT_FOUND,
                               count--, NULL);
               if (msg) free(msg);
            }

         }
      }
      else {
         spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_PROVIDER_NOT_FOUND,
                         count--, NULL);
      }
   }
   else {
      spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_INVALID_CLASS, 0,
                      NULL);
   }
   _SFCB_EXIT();
}



/* -------------
 * ---
 *      Method Provider support
 * ---
 * -------------
 */

static ProviderInfo *getMethodProvider(char *className, char *nameSpace)
{
   long type = METHOD_PROVIDER;
   const char *cls = className;
   ProviderInfo *info;
   CMPIStatus rc;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "getMethodProvider");
   
   if (methodProviderHt == NULL)
      methodProviderHt = UtilFactory->newHashTable(61,
         UtilHashTable_charKey | UtilHashTable_ignoreKeyCase);

   info = (ProviderInfo *) methodProviderHt->ft->get(methodProviderHt, className);
   if (info) _SFCB_RETURN(info);

   if (strcasecmp(className,"cim_indicationfilter")==0 ||
       strcasecmp(className,"cim_indicationsubscription")==0)
      _SFCB_RETURN(interOpProvInfoPtr);
      
   while (cls != NULL) {
      info = pReg->ft->getProvider(pReg, cls, type);
      if (info) {
         methodProviderHt->ft->put(methodProviderHt, className, info);
         _SFCB_RETURN(info);
      }
      else {
         CMPIConstClass *cc = (CMPIConstClass *) _getConstClass(nameSpace, cls, &rc);
         if (cc == NULL) {
            _SFCB_RETURN(NULL);
         }
         cls = cc->ft->getCharSuperClassName(cc);
      }
   }
   
   if (interopClass(className)) _SFCB_RETURN(forceNoProvInfoPtr);
   
   _SFCB_RETURN(defaultProvInfoPtr);
}

static void classProvider(int *requestor, OperationHdr * req)
{
   char *msg;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "classProvider");
   forkProvider(classProvInfoPtr, req, &msg);
   _SFCB_TRACE(1,("--- result %d-%lu to with %d-%lu",
      *requestor,getInode(*requestor),
      classProvInfoPtr->providerSockets.send,
      getInode(classProvInfoPtr->providerSockets.send)));
   
   spSendCtlResult(requestor, &classProvInfoPtr->providerSockets.send, MSG_X_PROVIDER,
                   0, getProvIds(classProvInfoPtr).ids);
                                   
   _SFCB_EXIT();
}

static void methProvider(int *requestor, OperationHdr * req)
{
   char *msg=NULL;
   int rc;
   char *className = (char *) req->className.data;
   char *nameSpace = (char *) req->nameSpace.data;
   ProviderInfo *info;
   
   _SFCB_ENTER(TRACE_PROVIDERMGR, "methProvider");
   if (strcmp(className, "$ClassProvider$") == 0)
      classProvider(requestor, req);
   else if ((info = getMethodProvider(className,nameSpace)) != NULL) {
      if (info->type!=FORCE_PROVIDER_NOTFOUND &&
          (rc = forkProvider(info, req, &msg)) == CMPI_RC_OK) {
         _SFCB_TRACE(1,("--- responding with  %s %p",info->providerName,info));
         spSendCtlResult(requestor, &info->providerSockets.send, MSG_X_PROVIDER,
                         0, getProvIds(info).ids);
      }                   
      else {
         spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_PROVIDER_NOT_FOUND,
                         0, NULL);
         if (msg) free(msg);
      }
   }
   else
      spSendCtlResult(requestor, &sfcbSockets.send, MSG_X_INVALID_CLASS, 0,
                      NULL);
   _SFCB_EXIT();
}

static int _methProvider(BinRequestContext * ctx, OperationHdr * req)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "_methProvider");
   char *className = (char *) req->className.data;
   char *nameSpace = (char *) req->nameSpace.data;
   ProviderInfo *info;
   int rc;
   char *msg;

   ctx->chunkedMode=ctx->xmlAs=0;
   if (strcmp(className, "$ClassProvider$") == 0) {
      forkProvider(classProvInfoPtr, req, &msg);
      ctx->provA.ids = getProvIds(classProvInfoPtr);
      ctx->provA.socket = classProvInfoPtr->providerSockets.send;
      ctx->pAs=NULL;
      _SFCB_RETURN(MSG_X_PROVIDER); 
   }
   else if (strcmp(className, "$InterOpProvider$") == 0) {
      forkProvider(interOpProvInfoPtr, req, &msg);
      ctx->provA.ids = getProvIds(interOpProvInfoPtr);
      ctx->provA.socket = interOpProvInfoPtr->providerSockets.send;
      ctx->pAs=NULL;
      _SFCB_RETURN(MSG_X_PROVIDER);
   }
   else if ((info = getMethodProvider(className,nameSpace)) != NULL) {
      if ((rc = forkProvider(info, req, &msg)) == CMPI_RC_OK) {
         ctx->provA.ids = getProvIds(info);
         ctx->provA.socket = info->providerSockets.send;
         ctx->pAs=NULL; 
         _SFCB_RETURN(MSG_X_PROVIDER);
      }
      else {
         free(msg);
         _SFCB_RETURN(MSG_X_PROVIDER_NOT_FOUND);
      }
   }
   _SFCB_RETURN(MSG_X_INVALID_CLASS);
}

static MgrHandler mHandlers[] = {
   {notSupported},              //dummy
   {classProvider},             //OPS_GetClass 1
   {instProvider},              //OPS_GetInstance 2
   {notSupported},              //OPS_DeleteClass 3
   {instProvider},              //OPS_DeleteInstance 4
   {notSupported},              //OPS_CreateClass 5
   {instProvider},              //OPS_CreateInstance 6
   {notSupported},              //OPS_ModifyClass 7
   {instProvider},              //OPS_ModifyInstance 8
   {classProvider},             //OPS_EnumerateClasses 9
   {classProvider},             //OPS_EnumerateClassNames 10
   {instProviderList},          //OPS_EnumerateInstances 11
   {instProviderList},          //OPS_EnumerateInstanceNames 12
   {instProviderList},          //OPS_ExecQuery 13
   {assocProviderList},         //OPS_Associators 14
   {assocProviderList},         //OPS_AssociatorNames 15
   {assocProviderList},         //OPS_References 16
   {assocProviderList},         //OPS_ReferenceNames 17
   {notSupported},              //OPS_GetProperty 18
   {notSupported},              //OPS_SetProperty 19
   {notSupported},              //OPS_GetQualifier 20
   {notSupported},              //OPS_SetQualifier 21
   {notSupported},              //OPS_DeleteQualifier 22
   {notSupported},              //OPS_EnumerateQualifiers 23
   {methProvider},              //OPS_InvokeMethod 24
   {NULL},
   {NULL},
#ifdef SFCB_INCL_INDICATION_SUPPORT
   {processIndProviderList},    //OPS_ProcessIndicationList 24
#else
   {notSupported},              //OPS_ProcessIndicationList 24
#endif   
};
      
void processProviderMgrRequests()
{
   int requestor;
   OperationHdr *req;
   unsigned long rl;
   int rc;
   char *cn, *ns;
 
   _SFCB_ENTER(TRACE_PROVIDERMGR, "processProviderMgrRequests");

   startUpProvider("root/interop","$ClassProvider$");
   
   if (interOpProvInfoPtr!=forceNoProvInfoPtr) {
     sleep(2);
     startUpProvider("root/interop","$InterOpProvider$");
   }   
      
   for (;;) {
      MgrHandler hdlr;

      _SFCB_TRACE(1,("--- Waiting for mgr request to %d ", sfcbSockets.receive));

      if ((rc = spRecvReq(&sfcbSockets.receive, &requestor, (void **) &req, &rl)) == 0) {
         req->nameSpace.data=(void*)((int)req->nameSpace.data+(char*)req);
         if (req->className.length)
            req->className.data=(void*)((int)req->className.data+(char*)req);
         else req->className.data=NULL;
         cn = (char *) req->className.data;
         ns = (char *) req->nameSpace.data;

        _SFCB_TRACE(1,("--- Mgr request for %s-%s (%d) from %d", req->nameSpace.data,
                req->className.data,req->type,requestor));
                
        hdlr = mHandlers[req->type];
        hdlr.handler(&requestor, req);
        _SFCB_TRACE(1,("--- Mgr request for %s-%s DONE", req->nameSpace.data,
              req->className.data));
         free(req);
      }
      else {
         _SFCB_ABORT();
      }
   }
   _SFCB_EXIT();
}

void closeProviderContext(BinRequestContext * ctx)
{ 
   if (ctx->pAs) free(ctx->pAs);
}

static void setInuseSem(void *id)
{
   ProvIds ids;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "setInuseSem");
   ids.ids=id;   
   semAcquire(sfcbSem,(ids.procId*3)+provProcGuardId+provProcBaseId);
   semAcquire(sfcbSem,(ids.procId*3)+provProcInuseId+provProcBaseId);
   semReleaseUnDo(sfcbSem,(ids.procId*3)+provProcInuseId+provProcBaseId);
   semRelease(sfcbSem,(ids.procId*3)+provProcGuardId+provProcBaseId);
   _SFCB_EXIT();
}


int getProviderContext(BinRequestContext * ctx, OperationHdr * ohdr)
{
   unsigned long int l;
   int rc, i, x;
   char *buf;
   ProvAddr *as;

   _SFCB_ENTER(TRACE_PROVIDERMGR, "getProviderContext");

   l = sizeof(*ohdr) + ohdr->nameSpace.length + ohdr->className.length;
   buf = (char *) malloc(l + 8);

   memcpy(buf, ohdr, sizeof(*ohdr));
   l = sizeof(*ohdr);
   memcpy(buf + l, ohdr->nameSpace.data, ohdr->nameSpace.length);
   ((OperationHdr *) buf)->nameSpace.data = (void *) l;
   l += ohdr->nameSpace.length;
   memcpy(buf + l, ohdr->className.data, ohdr->className.length);
   x = l;
   ((OperationHdr *) buf)->className.data = (void *) l;
   l += ohdr->className.length;
   
   _SFCB_TRACE(1,("--- Sending mgr request - to %d from %d", sfcbSockets.send,
                resultSockets.send));
   spSendReq(&sfcbSockets.send, &resultSockets.send, buf, l);
   free(buf);

   _SFCB_TRACE(1, ("--- Sending mgr request done"));
   
   ctx->rc =
      spRecvCtlResult(&resultSockets.receive, &ctx->provA.socket, &ctx->provA.ids.ids, &l);
   _SFCB_TRACE(1,("--- Provider socket: %d - %lu %d",ctx->provA.socket,getInode(ctx->provA.socket),currentProc));

   if (ctx->rc == MSG_X_PROVIDER) {
      _SFCB_TRACE(1,("--- Provider count: %d",l));
      setInuseSem(ctx->provA.ids.ids);
      ctx->pCount = l + 1;
      ctx->pAs = as = (ProvAddr*) malloc(sizeof(ProvAddr) * (l + 1));
      *as = ctx->provA;
      
      _SFCB_TRACE(1,("--- Provider socket: %d - %lu %d %lu %s",
         ctx->provA.socket,l,currentProc,getInode(ctx->provA.socket),ohdr->className));
  
      for (i = 1; l; i++) {
         rc = spRecvCtlResult(&resultSockets.receive, &as[i].socket,&as[i].ids.ids, &l);
         setInuseSem(as[i].ids.ids);
         _SFCB_TRACE(1,("--- getting provider socket: %lu %d",as[i].socket,getInode(as[i].socket),currentProc));
      }
   }

   else if (ctx->rc == MSG_X_EXTENDED_CTL_MSG) {
      ctx->rc = ctx->ctlXdata->code;
   }

   _SFCB_RETURN(ctx->rc);
}

BinResponseHdr *invokeProvider(BinRequestContext * ctx)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR | TRACE_CIMXMLPROC, "invokeProvider");
   int i, l, ol,rc;
   unsigned long size = ctx->bHdrSize;
   char *buf;
   BinRequestHdr *hdr = ctx->bHdr;
   BinResponseHdr *resp;
   int fromS;
   void *heapCtl=markHeap();

   hdr->provId = ctx->provA.ids.ids;
      
   for (l = size, i = 0; i < hdr->count; i++)
      l += hdr->object[i].length;

   buf = (char *) malloc(l + 8);
   
   if (ctx->noResp & 1) {
      hdr->options |= 1;
      _SFCB_TRACE(1, ("--- noResp set"));
   }
   
   memcpy(buf, hdr, size);
   for (l = size, i = 0; i < hdr->count; i++) {
      ol = hdr->object[i].length;
      switch (hdr->object[i].type) {
      case MSG_SEG_OBJECTPATH:
         getSerializedObjectPath((CMPIObjectPath *) hdr->object[i].data,
                                 buf + l);
         ((BinRequestHdr *) buf)->object[i].data = (void *) l;
         l += ol;
         break;
      case MSG_SEG_INSTANCE:
         getSerializedInstance((CMPIInstance *) hdr->object[i].data, buf + l);
         ((BinRequestHdr *) buf)->object[i].data = (void *) l;
         l += ol;
         break;
      case MSG_SEG_CHARS:
         memcpy(buf + l, hdr->object[i].data, ol);
         ((BinRequestHdr *) buf)->object[i].data = (void *) l;
         l += ol;
         break;
      case MSG_SEG_CONSTCLASS:
         getSerializedConstClass((CMPIConstClass *) hdr->object[i].data,
                                 buf + l);
         ((BinRequestHdr *) buf)->object[i].data = (void *) l;
         l += ol;
         break;
      case MSG_SEG_ARGS:
         getSerializedArgs((CMPIArgs *) hdr->object[i].data, buf + l);
         ((BinRequestHdr *) buf)->object[i].data = (void *) l;
         l += ol;
         break;
      default:
         fprintf(stderr,"--- bad invokeProvider request %d-%d\n", i,hdr->object[i].type);
         abort();
      }
   }

   _SFCB_TRACE(1, (
         "--- Sending Provider invocation request (%d-%p) - to %d-%lu from %d-%lu",
          hdr->operation,hdr->provId,ctx->provA.socket,getInode(ctx->provA.socket),
          resultSockets.send,
          getInode(resultSockets.send)));
          
   for (;;) {
      rc=spSendReq(&ctx->provA.socket, &resultSockets.send, buf, l);
      if (rc==-2) {
         fprintf(stderr,"need to reload provider\n");
         asm("int $3");
   //      reloadProviderRequest(ctx);
         exit(3);
      }
      else break;
   }

   free(buf);

   _SFCB_TRACE(1, ("--- Waiting for Provider response - from %d",resultSockets.receive));

   if (ctx->chunkedMode) {
      _SFCB_TRACE(1,("--- chunked mode"));
      resp=NULL;
      do {
         void *hc=markHeap();
         
         if (resp) free(resp);
         spRecvResult(&resultSockets.receive, &fromS, (void**) &resp, &size);
//         dump("result: ",resp,size); //asm("int $3");
         for (i = 0; i < resp->count; i++) {
            resp->object[i].data=(void*)((int)resp->object[i].data+(char*)resp);
         }
         
         ctx->rCount=1;
         _SFCB_TRACE(1,("--- writing chunk")); 
         ctx->chunkFncs->writeChunk(ctx, resp);
         _SFCB_TRACE(1,("--- back from writing chunk"));
         if (resp->moreChunks) spSendAck(resultSockets.receive);
         
         releaseHeap(hc);
         
      } while (resp->moreChunks);
      if (resp) free(resp);
   }

   else if ((ctx->noResp & 1)==0) {
      spRecvResult(&resultSockets.receive, &fromS, (void **) &resp, &size);

     ctx->rCount=ctx->pCount;
     if (resp->rvValue) {
        if (resp->rv.type==CMPI_chars)
           resp->rv.value.chars=(int)resp->rvEnc.data+(char*)resp;
     }
     for (i = 0; i < resp->count; i++) {
         resp->object[i].data=(void*)((int)resp->object[i].data+(char*)resp);   
     }    
   }
   else {
      _SFCB_TRACE(1,("--- waiting for response skipped")); 
      resp=NULL;
   }   
   
   releaseHeap(heapCtl);
   _SFCB_RETURN(resp);
}

BinResponseHdr **invokeProviders(BinRequestContext * binCtx, int *err,
                                 int *count)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR | TRACE_CIMXMLPROC, "invokeProviders");
   BinResponseHdr **resp;
   int i;

   resp = malloc(sizeof(BinResponseHdr *) * (binCtx->pCount));
   *err = 0;
   *count = 0;
   binCtx->pDone=1;

   _SFCB_TRACE(1, ("--- %d providers", binCtx->pCount));
   for (i = 0; i < binCtx->pCount; i++,binCtx->pDone++) {
      _SFCB_TRACE(1, ("--- Calling provider ..."));
      binCtx->provA = binCtx->pAs[i];
      resp[i] = invokeProvider(binCtx);
      _SFCB_TRACE(1, ("--- back from calling provider"));
      *count += resp[i]->count;
      resp[i]->rc--;
      if (*err == 0 && resp[i]->rc != 0) *err = i+1;
   }
   _SFCB_RETURN(resp);
}

extern void lockUpCall(CMPIBroker* mb);
extern void unlockUpCall(CMPIBroker* mb);

CMPIConstClass *getConstClass(const char *ns, const char *cn)
{
   CMPIObjectPath *path;
   CMPIConstClass *ccl;
   CMPIStatus rc;
   GetClassReq sreq = BINREQ(OPS_GetClass,2);
   BinResponseHdr *resp;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_GetClass, 2 };
   int irc;
   
   _SFCB_ENTER(TRACE_PROVIDERMGR, "getConstClass");
   
   path = TrackedCMPIObjectPath(ns, cn, &rc);
   sreq.principle = setCharsMsgSegment("$$");
   sreq.objectPath = setObjectPathMsgSegment(path);

   req.nameSpace = setCharsMsgSegment((char *) ns);
   req.className = setCharsMsgSegment((char *) cn);

   memset(&binCtx,0,sizeof(BinRequestContext));
   binCtx.oHdr = &req;
   binCtx.bHdr = &sreq.hdr;
   binCtx.bHdrSize = sizeof(sreq);
   binCtx.chunkedMode=binCtx.xmlAs=binCtx.noResp=0;

   lockUpCall(Broker);
   
   irc = getProviderContext(&binCtx, &req);

   if (irc) {
      _SFCB_TRACE(1, ("--- Invoking Provider"));
      resp = invokeProvider(&binCtx);
      resp->rc--;
      if (resp->rc == CMPI_RC_OK) {
         ccl = relocateSerializedConstClass(resp->object[0].data);
         ccl = ccl->ft->clone(ccl, NULL);
         tool_mm_add(ccl);
      }
      else ccl=NULL;
   }
   else ccl = NULL;
   
   unlockUpCall(Broker);

   free(resp);
   closeProviderContext(&binCtx);

   _SFCB_RETURN(ccl);
}

static CMPIConstClass *_getConstClass(const char *ns, const char *cn, CMPIStatus *st)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "_getConstClass");

   CMPIObjectPath *path;
   CMPIConstClass *ccl = NULL;
   GetClassReq sreq = BINREQ(OPS_GetClass, 2);
   BinResponseHdr *resp;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_GetClass, 2 };
   int irc;
   char *msg;
   
   path = NewCMPIObjectPath(ns, cn, st);
   sreq.objectPath = setObjectPathMsgSegment(path);
   sreq.principle = setCharsMsgSegment("$$");

   req.nameSpace = setCharsMsgSegment((char *) ns);
   req.className = setCharsMsgSegment((char *) cn);

   forkProvider(classProvInfoPtr, &req, &msg);
   
   memset(&binCtx,0,sizeof(BinRequestContext));
   binCtx.oHdr = &req;
   binCtx.bHdr = &sreq.hdr;
   binCtx.bHdrSize = sizeof(sreq);
   binCtx.provA.ids=getProvIds(classProvInfoPtr);
   binCtx.chunkedMode=binCtx.xmlAs=binCtx.noResp=0;

   binCtx.provA.socket = classProvInfoPtr->providerSockets.send;
   irc = MSG_X_PROVIDER;

   if (irc) {
      _SFCB_TRACE(1, ("--- Invoking ClassProvider for %s %s",ns,cn));
      resp = invokeProvider(&binCtx);
      resp->rc--;
      st->rc=resp->rc;
      if (resp->rc == CMPI_RC_OK) {
         ccl = relocateSerializedConstClass(resp->object[0].data);
         ccl = ccl->ft->clone(ccl, NULL);
         tool_mm_add(ccl);
      }
   }
   _SFCB_TRACE(1, ("--- Invoking ClassProvider for %s %s rc: %d",ns,cn,resp->rc));
   
   path->ft->release(path);
   free(resp);

   _SFCB_RETURN(ccl);
}

static CMPIData localInvokeMethod(BinRequestContext * binCtx,
                                  CMPIObjectPath * path, char *method,
                                  CMPIArgs * in, CMPIArgs ** out,
                                  CMPIStatus * rc, int noResp)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "localInvokeMethod");

   InvokeMethodReq sreq = BINREQ(OPS_InvokeMethod, 5);
   OperationHdr req = { OPS_InvokeMethod, 1 };
   CMPIData data = { 0, CMPI_nullValue, {0} };
   *out=NULL;
   BinResponseHdr *resp=NULL;
   
   sreq.in = setArgsMsgSegment(in);
   sreq.out = setArgsMsgSegment(NULL);
   sreq.objectPath = setObjectPathMsgSegment(path);
   sreq.method = setCharsMsgSegment(method);
   sreq.principle = setCharsMsgSegment("$$");

   binCtx->oHdr = &req;
   binCtx->bHdr = &sreq.hdr;
   binCtx->bHdrSize = sizeof(sreq);
   binCtx->chunkedMode=binCtx->xmlAs=0;
   binCtx->noResp=noResp;

   if (noResp==0) {
      resp = invokeProvider(binCtx);
      resp->rc--;
      if (rc) rc->rc = resp->rc;
      if (resp->rc == CMPI_RC_OK) {
         *out = relocateSerializedArgs(resp->object[0].data);
         *out = (*out)->ft->clone(*out, NULL);
         data=resp->rv;
         // check for chars
      }
   }
   else {
       resp = invokeProvider(binCtx);
       if (rc) rc->rc = CMPI_RC_OK;
   }
   
   if (resp) free(resp);

   closeProviderContext(binCtx);
   
   _SFCB_RETURN(data);
}

int isChild(const char *ns, const char *parent, const char* child)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "isChild");

   CMPIObjectPath *path;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_InvokeMethod, 1 };
   CMPIArgs *in = NewCMPIArgs(NULL);
   CMPIArgs *out = NULL;
   CMPIStatus rc;
   int irc;

   memset(&binCtx,0,sizeof(BinRequestContext));
   CMAddArg(in, "child", child, CMPI_chars);
   path = NewCMPIObjectPath(ns, parent, &rc);
   req.nameSpace = setCharsMsgSegment((char *) ns);
   req.className = setCharsMsgSegment("$ClassProvider$");

   irc = _methProvider(&binCtx, &req);

   if (irc) {
      localInvokeMethod(&binCtx, path, "ischild", in, &out, &rc,0);
      irc=(rc.rc==CMPI_RC_OK);
   }
   else irc=0;
   CMRelease(path);
   CMRelease(in);
   
   _SFCB_RETURN(irc);
}

static int startUpProvider(const char* ns, const char *name)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "startUpProvider");

   CMPIObjectPath *path;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_InvokeMethod, 1 };
   CMPIArgs *in = NewCMPIArgs(NULL);
   CMPIArgs *out = NULL;
   CMPIStatus rc;
   int irc;

   memset(&binCtx,0,sizeof(BinRequestContext));
   path = NewCMPIObjectPath(ns, "$$", &rc);
   req.nameSpace = setCharsMsgSegment((char*)ns);
   req.className = setCharsMsgSegment((char*)name);

   irc = _methProvider(&binCtx, &req);

   if (irc) {
      localInvokeMethod(&binCtx, path, "_startup", in, &out, &rc, 1);
      irc=(rc.rc==CMPI_RC_OK);
   }
   else irc=0;
   
   CMRelease(path);
   CMRelease(in);
   
   _SFCB_RETURN(irc);
}

static UtilList *_getConstClassChildren(const char *ns, const char *cn)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "_getConstClassChildren");

   CMPIObjectPath *path;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_InvokeMethod, 1 };
   CMPIArgs *in = NewCMPIArgs(NULL);
   CMPIArgs *out = NULL;
   CMPIData data;
   CMPIArray *ar;
   CMPIStatus rc;
   UtilList *ul = NULL;
   int i, m, irc;
   CMPIString *str;

   memset(&binCtx,0,sizeof(BinRequestContext));
   CMAddArg(in, "class", cn, CMPI_chars);
   path = NewCMPIObjectPath(ns, cn, &rc);
   req.nameSpace = setCharsMsgSegment((char *) ns);
   req.className = setCharsMsgSegment("$ClassProvider$");

   irc = _methProvider(&binCtx, &req);

   if (irc) {
      data = localInvokeMethod(&binCtx, path, "getchildren", in, &out, &rc, 0);
      if (out) {
         ar = CMGetArg(out, "children", &rc).value.array;
         ul = UtilFactory->newList();
         for (i = 0, m = CMGetArrayCount(ar, NULL); i < m; i++) {
            str=CMGetArrayElementAt(ar, i, NULL).value.string;
            if (str && str->hdl) ul->ft->append(ul,str->hdl);
         }                  
      }
   }
   CMRelease(path);
   if (out) CMRelease(out);
   CMRelease(in);

   _SFCB_RETURN(ul);
}

static UtilList *_getAssocClassNames(const char *ns)
{
   _SFCB_ENTER(TRACE_PROVIDERMGR, "_getAssocClassNames");

   CMPIObjectPath *path;
   BinRequestContext binCtx;
   OperationHdr req = { OPS_InvokeMethod, 1 };
   CMPIArgs *in = NewCMPIArgs(NULL);
   CMPIArgs *out = NULL;
   CMPIData data;
   CMPIArray *ar;
   CMPIStatus rc;
   UtilList *ul = NULL;
   int i, m, irc;

   _SFCB_TRACE(1,("--- for %s",ns));
   path = NewCMPIObjectPath(ns, "$ClassProvider$", &rc);
   req.nameSpace = setCharsMsgSegment((char *) ns);
   req.className = setCharsMsgSegment("$ClassProvider$");

   memset(&binCtx,0,sizeof(BinRequestContext));
   irc = _methProvider(&binCtx, &req);

   if (irc) {
      data = localInvokeMethod(&binCtx, path, "getassocs", in, &out, &rc,0);
      if (out) {
         ar = CMGetArg(out, "assocs", &rc).value.array;
         ul = UtilFactory->newList();
         for (i = 0, m = CMGetArrayCount(ar, NULL); i < m; i++) {
            char *name=CMGetArrayElementAt(ar,i,NULL).value.string->hdl;
            if (name) ul->ft->append(ul,name);
            _SFCB_TRACE(1,("--- assoc %s",name));
         }
      }
   }
   CMRelease(path);
   if (out) CMRelease(out);
   CMRelease(in);

   _SFCB_RETURN(ul);
}
