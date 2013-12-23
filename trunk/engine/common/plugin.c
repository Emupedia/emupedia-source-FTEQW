//This file should be easily portable.
//The biggest strength of this plugin system is that ALL interactions are performed via
//named functions, this makes it *really* easy to port plugins from one engine to annother.

#include "quakedef.h"
#include "fs.h"

#define PLUG_NONE 0
#define PLUG_NATIVE 1
#define PLUG_QVM 2
#define PLUG_EITHER 3

#ifdef PLUGINS

cvar_t plug_sbar = SCVAR("plug_sbar", "1");
cvar_t plug_loaddefault = SCVAR("plug_loaddefault", "1");

#ifdef GLQUAKE
#include "glquake.h"
#endif

//custom plugin builtins.
typedef qintptr_t (EXPORT_FN *Plug_Builtin_t)(void *offset, quintptr_t mask, const qintptr_t *arg);
void Plug_RegisterBuiltin(char *name, Plug_Builtin_t bi, int flags);
#define PLUG_BIF_DLLONLY	1
#define PLUG_BIF_QVMONLY	2
#define PLUG_BIF_NEEDSRENDERER 4

#include "netinc.h"

typedef struct plugin_s {
	char *name;
	vm_t *vm;

	int blockcloses;

	void *inputptr;
	unsigned int inputbytes;

	int tick;
	int executestring;
#ifndef SERVERONLY
	int consolelink;
	int consolelinkmouseover;
	int conexecutecommand;
	int menufunction;
	int sbarlevel[3];	//0 - main sbar, 1 - supplementry sbar sections (make sure these can be switched off), 2 - overlays (scoreboard). menus kill all.
	int reschange;

	//protocol-in-a-plugin
	int connectionlessclientpacket;

	//called to discolour console input text if they spelt it wrongly
	int spellcheckmaskedtext;
#endif
	int svmsgfunction;
	int chatmsgfunction;
	int centerprintfunction;
	int shutdown;

	struct plugin_s *next;
} plugin_t;

void Plug_SubConsoleCommand(console_t *con, char *line);

plugin_t *currentplug;



#ifndef SERVERONLY
#include "cl_plugin.inc"
#else
void Plug_Client_Init(void){}
void Plug_Client_Close(plugin_t *plug) {}
#endif


void Plug_Init(void);
void Plug_Close(plugin_t *plug);

void Plug_Tick(void);
qboolean Plugin_ExecuteString(void);


static plugin_t *plugs;


typedef struct {
	char *name;
	Plug_Builtin_t func;
	int flags;
} Plug_Plugins_t;
Plug_Plugins_t *plugbuiltins;
int numplugbuiltins;

void Plug_RegisterBuiltin(char *name, Plug_Builtin_t bi, int flags)
{
	//randomize the order a little.
	int newnum;

	newnum = (rand()%128)+1;
	while(newnum < numplugbuiltins && plugbuiltins[newnum].func)
		newnum+=128;

	if (newnum >= numplugbuiltins)
	{
		int newbuiltins = newnum+128;
		plugbuiltins = BZ_Realloc(plugbuiltins, sizeof(Plug_Plugins_t)*newbuiltins);
		memset(plugbuiltins + numplugbuiltins, 0, sizeof(Plug_Plugins_t)*(newbuiltins - numplugbuiltins));
		numplugbuiltins = newbuiltins;
	}

	//got an empty number.
	Con_DPrintf("%s: %i\n", name, newnum);
	plugbuiltins[newnum].name = name;
	plugbuiltins[newnum].func = bi;
	plugbuiltins[newnum].flags = flags;
}

static qintptr_t VARGS Plug_GetNativePointer(void *offset, quintptr_t mask, const qintptr_t *args)
{
	char *p = (char *)VM_POINTER(args[0]);
#ifdef SUPPORT_ICE
	if (!strcmp(p, ICE_API_CURRENT))
		return (qintptr_t)&iceapi;
#endif

	return (qintptr_t)NULL;
}

/*
static void Plug_RegisterBuiltinIndex(char *name, Plug_Builtin_t bi, int flags, int index)	//I d
{
	//randomize the order a little.
	int newnum;

	newnum = rand()%128;
	while(newnum+1 < numplugbuiltins && plugbuiltins[newnum+1].func)
		newnum+=128;

	newnum++;

	if (newnum >= numplugbuiltins)
	{
		numplugbuiltins = newnum+128;
		plugbuiltins = BZ_Realloc(plugbuiltins, sizeof(Plug_Plugins_t)*numplugbuiltins);
	}

	//got an empty number.
	plugbuiltins[newnum].name = name;
	plugbuiltins[newnum].func = bi;
}
*/

static qintptr_t Plug_FindBuiltin(qboolean native, char *p)
{
	int i;
	for (i = 0; i < numplugbuiltins; i++)
		if (plugbuiltins[i].name)
			if (p && !strcmp(plugbuiltins[i].name, p))
			{
				if (!native && plugbuiltins[i].flags & PLUG_BIF_DLLONLY)
					return 0;	//block it, if not native
				if (native && plugbuiltins[i].flags & PLUG_BIF_QVMONLY)
					return 0;	//block it, if not native
				return -i;
			}

	return 0;
}
static qintptr_t VARGS Plug_GetBuiltin(void *offset, quintptr_t mask, const qintptr_t *args)
{
	char *p = (char *)VM_POINTER(args[0]);
	return Plug_FindBuiltin(!offset, p);
}

static int Plug_SystemCallsVM(void *offset, quintptr_t mask, int fn, const int *arg)
{
#if FTE_WORDSIZE == 32
	#define args arg
#else
	qintptr_t args[9];

	args[0]=arg[0];
	args[1]=arg[1];
	args[2]=arg[2];
	args[3]=arg[3];
	args[4]=arg[4];
	args[5]=arg[5];
	args[6]=arg[6];
	args[7]=arg[7];
	args[8]=arg[8];
#endif

	fn = fn+1;

	if (fn>=0 && fn < numplugbuiltins && plugbuiltins[fn].func!=NULL)
		return plugbuiltins[fn].func(offset, mask, (qintptr_t*)args);
#undef args
	Sys_Error("QVM Plugin tried calling invalid builtin %i", fn);
	return 0;
}

//I'm not keen on this.
//but dlls call it without saying what sort of vm it comes from, so I've got to have them as specifics
static qintptr_t EXPORT_FN Plug_SystemCallsNative(qintptr_t arg, ...)
{
	qintptr_t args[9];
	va_list argptr;

	va_start(argptr, arg);
	args[0]=va_arg(argptr, qintptr_t);
	args[1]=va_arg(argptr, qintptr_t);
	args[2]=va_arg(argptr, qintptr_t);
	args[3]=va_arg(argptr, qintptr_t);
	args[4]=va_arg(argptr, qintptr_t);
	args[5]=va_arg(argptr, qintptr_t);
	args[6]=va_arg(argptr, qintptr_t);
	args[7]=va_arg(argptr, qintptr_t);
	args[8]=va_arg(argptr, qintptr_t);
	va_end(argptr);

	arg = -arg;

	if (arg>=0 && arg < numplugbuiltins && plugbuiltins[arg].func)
		return plugbuiltins[arg].func(NULL, ~0, args);

	Sys_Error("DLL Plugin tried calling invalid builtin %i", (int)arg);
	return 0;
}


plugin_t *Plug_Load(char *file, int type)
{
	plugin_t *newplug;

	for (newplug = plugs; newplug; newplug = newplug->next)
	{
		if (!stricmp(newplug->name, file))
			return newplug;
	}

	newplug = Z_Malloc(sizeof(plugin_t)+strlen(file)+1);
	newplug->name = (char*)(newplug+1);
	strcpy(newplug->name, file);

	if (!newplug->vm && (type & PLUG_NATIVE))
		newplug->vm = VM_Create(va("fteplug_%s", file), Plug_SystemCallsNative, NULL);
	if (!newplug->vm && (type & PLUG_QVM))
		newplug->vm = VM_Create(file, NULL, Plug_SystemCallsVM);

	currentplug = newplug;
	if (newplug->vm)
	{
		Con_TPrintf("Created plugin %s\n", file);

		newplug->next = plugs;
		plugs = newplug;

		if (!VM_Call(newplug->vm, 0, Plug_FindBuiltin(true, "Plug_GetEngineFunction")))
		{
			Plug_Close(newplug);
			return NULL;
		}

#ifndef SERVERONLY
		if (newplug->reschange)
			VM_Call(newplug->vm, newplug->reschange, vid.width, vid.height);
#endif
	}
	else
	{
		Z_Free(newplug);
		newplug = NULL;
	}
	currentplug = NULL;

	return newplug;
}

static int QDECL Plug_Emumerated (const char *name, int size, void *param, searchpathfuncs_t *spath)
{
	char vmname[MAX_QPATH];
	Q_strncpyz(vmname, name, sizeof(vmname));
	vmname[strlen(vmname) - strlen(param)] = '\0';
	if (!Plug_Load(vmname, PLUG_QVM))
		Con_Printf("Couldn't load plugin %s\n", vmname);

	return true;
}
static int QDECL Plug_EnumeratedRoot (const char *name, int size, void *param, searchpathfuncs_t *spath)
{
	char vmname[MAX_QPATH];
	int len;
	char *dot;
	if (!strncmp(name, "fteplug_", 8))
		name += 8;
	Q_strncpyz(vmname, name, sizeof(vmname));
	len = strlen(vmname);
	len -= strlen(ARCH_CPU_POSTFIX ARCH_DL_POSTFIX);
	if (!strcmp(vmname+len, ARCH_CPU_POSTFIX ARCH_DL_POSTFIX))
		vmname[len] = 0;
	else
	{
		dot = strchr(vmname, '.');
		if (dot)
			*dot = 0;
	}
	if (!Plug_Load(vmname, PLUG_NATIVE))
		Con_Printf("Couldn't load plugin %s\n", vmname);

	return true;
}

static qintptr_t VARGS Plug_Con_Print(void *offset, quintptr_t mask, const qintptr_t *arg)
{
//	if (qrenderer == QR_NONE)
//		return false;
	Con_Printf("%s", (char*)VM_POINTER(arg[0]));
	return 0;
}
static qintptr_t VARGS Plug_Sys_Error(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	Sys_Error("%s", (char*)offset+arg[0]);
	return 0;
}
static qintptr_t VARGS Plug_Sys_Milliseconds(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	return Sys_DoubleTime()*1000;
}
static qintptr_t VARGS Plug_ExportToEngine(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name = (char*)VM_POINTER(arg[0]);
	unsigned int functionid = VM_LONG(arg[1]);

	if (!strcmp(name, "Tick"))					//void(int realtime)
		currentplug->tick = functionid;
	else if (!strcmp(name, "ExecuteCommand"))	//bool(isinsecure)
		currentplug->executestring = functionid;
	else if (!strcmp(name, "Shutdown"))			//void()
		currentplug->shutdown = functionid;
#ifndef SERVERONLY
	else if (!strcmp(name, "ConsoleLink"))
		currentplug->consolelink = functionid;
	else if (!strcmp(name, "ConsoleLinkMouseOver"))
		currentplug->consolelinkmouseover = functionid;
	else if (!strcmp(name, "ConExecuteCommand"))
		currentplug->conexecutecommand = functionid;
	else if (!strcmp(name, "MenuEvent"))
		currentplug->menufunction = functionid;
	else if (!strcmp(name, "UpdateVideo"))
		currentplug->reschange = functionid;
	else if (!strcmp(name, "SbarBase"))			//basic SBAR.
		currentplug->sbarlevel[0] = functionid;
	else if (!strcmp(name, "SbarSupplement"))	//supplementry stuff - teamplay
		currentplug->sbarlevel[1] = functionid;
	else if (!strcmp(name, "SbarOverlay"))		//overlay - scoreboard type stuff.
		currentplug->sbarlevel[2] = functionid;
	else if (!strcmp(name, "ConnectionlessClientPacket"))
		currentplug->connectionlessclientpacket = functionid;
	else if (!strcmp(name, "ServerMessageEvent"))
		currentplug->svmsgfunction = functionid;
	else if (!strcmp(name, "ChatMessageEvent"))
		currentplug->chatmsgfunction = functionid;
	else if (!strcmp(name, "CenterPrintMessage"))
		currentplug->centerprintfunction = functionid;
	else if (!strcmp(name, "SpellCheckMaskedText"))
		currentplug->spellcheckmaskedtext = functionid;
#endif
	else
		return 0;
	return 1;
}

//retrieve a plugin's name
static qintptr_t VARGS Plug_GetPluginName(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int plugnum = VM_LONG(arg[0]);
	plugin_t *plug;
	//int plugnum (0 for current), char *buffer, int bufferlen

	if (VM_OOB(arg[1], arg[2]))
		return false;

	if (plugnum <= 0)
	{
		Q_strncpyz(VM_POINTER(arg[1]), currentplug->name, VM_LONG(arg[2]));
		return true;
	}

	for (plug = plugs; plug; plug = plug->next)
	{
		if (--plugnum == 0)
		{
			Q_strncpyz(VM_POINTER(arg[1]), plug->name, VM_LONG(arg[2]));
			return true;
		}
	}
	return false;
}

static qintptr_t VARGS Plug_ExportNative(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *func;
	char *name = (char*)VM_POINTER(arg[0]);
	arg++;

	func = ((void**)arg)[0];

	if (!strcmp(name, "UnsafeClose"))
	{
		//not used by the engine, but stops the user from being able to unload the plugin.
		//this is useful for certain things, like if the plugin uses some external networking or direct disk access or whatever.
		currentplug->blockcloses++;
	}
	else if (!strncmp(name, "FS_RegisterArchiveType_", 23))	//module as in pak/pk3
	{
		FS_RegisterFileSystemType(currentplug, name+23, func, true);
	}
	/*
	else if (!strncmp(name, "S_OutputDriver"))	//a sound driver (takes higher priority over the built-in ones)
	{
		S_RegisterOutputDriver(name + 13, func);
		currentplug->blockcloses++;
	}
	*/
	/*
	else if (!strncmp(name, "VID_DisplayDriver"))	//a video driver, loaded by name as given by vid_renderer
	{
		FS_RegisterModuleDriver(, func);
		currentplug->blockcloses++;
	}
	*/
#if defined(PLUGINS) && !defined(NOMEDIA) && !defined(SERVERONLY)
	else if (!strcmp(name, "Media_VideoDecoder"))
	{
		Media_RegisterDecoder(currentplug, func);
//		currentplug->blockcloses++;
	}
	else if (!strcmp(name, "Media_VideoEncoder"))
	{
		Media_RegisterEncoder(currentplug, func);
//		currentplug->blockcloses++;
	}
#endif

#ifndef SERVERONLY
	else if (!strcmp(name, "S_LoadSound"))	//a hook for loading extra types of sound (wav, mp3, ogg, midi, whatever you choose to support)
	{
		S_RegisterSoundInputPlugin((void*)func);
		currentplug->blockcloses++;
	}
#endif
	else
		return 0;
	return 1;
}

typedef struct {
	//Make SURE that the engine has resolved all cvar pointers into globals before this happens.
	plugin_t *plugin;
	cvar_t *var;
} plugincvararray_t;
int plugincvararraylen;
plugincvararray_t *plugincvararray;
//qhandle_t Cvar_Register (char *name, char *defaultval, int flags, char *grouphint);
static qintptr_t VARGS Plug_Cvar_Register(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name = VM_POINTER(arg[0]);
	char *defaultvalue = VM_POINTER(arg[1]);
	unsigned int flags = VM_LONG(arg[2]);
	char *groupname = VM_POINTER(arg[3]);
	cvar_t *var;
	int i;

	var = Cvar_Get(name, defaultvalue, flags&1, groupname);

	for (i = 0; i < plugincvararraylen; i++)
	{
		if (!plugincvararray[i].var)
		{	//hmm... a gap...
			plugincvararray[i].plugin = currentplug;
			plugincvararray[i].var = var;
			return i;
		}
	}

	i = plugincvararraylen;
	plugincvararraylen++;
	plugincvararray = BZ_Realloc(plugincvararray, (plugincvararraylen)*sizeof(plugincvararray_t));
	plugincvararray[i].plugin = currentplug;
	plugincvararray[i].var = var;
	return i;
}
//int Cvar_Update, (qhandle_t handle, int modificationcount, char *stringv, float *floatv));	//stringv is 256 chars long, don't expect this function to do anything if modification count is unchanged.
static qintptr_t VARGS Plug_Cvar_Update(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int handle;
//	int modcount;
	char *stringv;	//255 bytes long.
	float *floatv;
	cvar_t *var;
	handle = VM_LONG(arg[0]);
	if (handle < 0 || handle >= plugincvararraylen)
		return 0;
	if (plugincvararray[handle].plugin != currentplug)
		return 0;	//I'm not letting you know what annother plugin has registered.

	if (VM_OOB(arg[2], 256) || VM_OOB(arg[3], 4))	//Oi, plugin - you screwed up
		return 0;

	//modcount = VM_LONG(arg[1]);	//for future optimisation
	stringv = VM_POINTER(arg[2]);
	floatv = VM_POINTER(arg[3]);

	var = plugincvararray[handle].var;

	//if (var->modified != modcount)	//for future optimisation
	{
		strcpy(stringv, var->string);
		*floatv = var->value;
	}
	return var->modified;
}

//void Cmd_Args(char *buffer, int buffersize)
static qintptr_t VARGS Plug_Cmd_Args(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *buffer = (char*)VM_POINTER(arg[0]);
	char *args;
	args = Cmd_Args();
	if (strlen(args)+1>arg[1])
		return 0;
	strcpy(buffer, args);
	return 1;
}
//void Cmd_Argv(int num, char *buffer, int buffersize)
static qintptr_t VARGS Plug_Cmd_Argv(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *buffer = (char*)VM_POINTER(arg[1]);
	char *args;
	args = Cmd_Argv(arg[0]);
	if (strlen(args)+1>arg[2])
		return 0;
	strcpy(buffer, args);
	return 1;
}
//int Cmd_Argc(void)
static qintptr_t VARGS Plug_Cmd_Argc(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	return Cmd_Argc();
}

//void Cvar_SetString (char *name, char *value);
static qintptr_t VARGS Plug_Cvar_SetString(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name = VM_POINTER(arg[0]),
		*value = VM_POINTER(arg[1]);
	cvar_t *var = Cvar_Get(name, value, 0, "Plugin vars");
	if (var)
	{
		Cvar_Set(var, value);
		return 1;
	}

	return 0;
}

//void Cvar_SetFloat (char *name, float value);
static qintptr_t VARGS Plug_Cvar_SetFloat(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name = VM_POINTER(arg[0]);
	float value = VM_FLOAT(arg[1]);
	cvar_t *var = Cvar_Get(name, "", 0, "Plugin vars");	//"" because I'm lazy
	if (var)
	{
		Cvar_SetValue(var, value);
		return 1;
	}

	return 0;
}

//void Cvar_GetFloat (char *name);
static qintptr_t VARGS Plug_Cvar_GetFloat(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name = VM_POINTER(arg[0]);
	int ret;
	cvar_t *var;
#ifndef CLIENTONLY
	if (!strcmp(name, "sv.state"))
		VM_FLOAT(ret) = sv.state;
	else
#endif
	{
		var = Cvar_Get(name, "", 0, "Plugin vars");
		if (var)
		{
			VM_FLOAT(ret) = var->value;
		}
		else
			VM_FLOAT(ret) = 0;
	}
	return ret;
}

//qboolean Cvar_GetString (char *name, char *retstring, int sizeofretstring);
static qintptr_t VARGS Plug_Cvar_GetString(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *name, *ret;
	int retsize;
	cvar_t *var;
	if (VM_OOB(arg[1], arg[2]))
	{
		return false;
	}

	name = VM_POINTER(arg[0]);
	ret = VM_POINTER(arg[1]);
	retsize = VM_LONG(arg[2]);

	if (!strcmp(name, "sv.mapname"))
	{
#ifdef CLIENTONLY
		Q_strncpyz(ret, "", retsize);
#else
		Q_strncpyz(ret, sv.name, retsize);
#endif
	}
	else
	{
		var = Cvar_Get(name, "", 0, "Plugin vars");
		if (strlen(var->name)+1 > retsize)
			return false;

		strcpy(ret, var->string);
	}

	return true;
}

//void Cmd_AddText (char *text, qboolean insert);	//abort the entire engine.
static qintptr_t VARGS Plug_Cmd_AddText(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	if (VM_LONG(arg[1]))
		Cbuf_InsertText(VM_POINTER(arg[0]), RESTRICT_LOCAL, false);
	else
		Cbuf_AddText(VM_POINTER(arg[0]), RESTRICT_LOCAL);

	return 1;
}

int plugincommandarraylen;
typedef struct {
	plugin_t *plugin;
	char command[64];
} plugincommand_t;
plugincommand_t *plugincommandarray;
void Plug_Command_f(void)
{
	int i;
	char *cmd = Cmd_Argv(0);
	plugin_t *oldplug = currentplug;
	for (i = 0; i < plugincommandarraylen; i++)
	{
		if (!plugincommandarray[i].plugin)
			continue;	//don't check commands who's owners died.

		if (stricmp(plugincommandarray[i].command, cmd))	//not the right command
			continue;

		currentplug = plugincommandarray[i].plugin;

		if (currentplug->executestring)
			VM_Call(currentplug->vm, currentplug->executestring, Cmd_IsInsecure(), 0, 0, 0);
		break;
	}

	currentplug = oldplug;
}

static qintptr_t VARGS Plug_Cmd_AddCommand(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int i;
	char *name = VM_POINTER(arg[0]);
	for (i = 0; i < plugincommandarraylen; i++)
	{
		if (!plugincommandarray[i].plugin)
			break;
		if (plugincommandarray[i].plugin == currentplug)
		{
			if (!strcmp(name, plugincommandarray[i].command))
				return true;	//already registered
		}
	}
	if (i == plugincommandarraylen)
	{
		plugincommandarraylen++;
		plugincommandarray = BZ_Realloc(plugincommandarray, plugincommandarraylen*sizeof(plugincommand_t));
	}

	Q_strncpyz(plugincommandarray[i].command, name, sizeof(plugincommandarray[i].command));
	if (!Cmd_AddCommand(plugincommandarray[i].command, Plug_Command_f))
		return false;
	plugincommandarray[i].plugin = currentplug;	//worked
	return true;
}
static void VARGS Plug_FreeConCommands(plugin_t *plug)
{
	int i;
	for (i = 0; i < plugincommandarraylen; i++)
	{
		if (plugincommandarray[i].plugin == plug)
		{
			plugincommandarray[i].plugin = NULL;
			Cmd_RemoveCommand(plugincommandarray[i].command);
		}
	}
}

typedef enum{
	STREAM_NONE,
	STREAM_SOCKET,
	STREAM_VFS
} plugstream_e;

typedef struct {
	plugin_t *plugin;
	plugstream_e type;
	int socket;
	vfsfile_t *vfs;
	struct {
		char filename[MAX_QPATH];
		qbyte *buffer;
		int buflen;
		int curlen;
		int curpos;
	} file;
} pluginstream_t;
pluginstream_t *pluginstreamarray;
int pluginstreamarraylen;

static int Plug_NewStreamHandle(plugstream_e type)
{
	int i;
	for (i = 0; i < pluginstreamarraylen; i++)
	{
		if (!pluginstreamarray[i].plugin)
			break;
	}
	if (i >= pluginstreamarraylen)
	{
		pluginstreamarraylen=i+16;
		pluginstreamarray = BZ_Realloc(pluginstreamarray, pluginstreamarraylen*sizeof(pluginstream_t));
	}

	memset(&pluginstreamarray[i], 0, sizeof(pluginstream_t));
	pluginstreamarray[i].plugin = currentplug;
	pluginstreamarray[i].type = type;
	pluginstreamarray[i].socket = -1;
	pluginstreamarray[i].file.buffer = NULL;
	*pluginstreamarray[i].file.filename = '\0';

	return i;
}

#ifndef NACL
//EBUILTIN(int, NET_TCPListen, (char *ip, int port, int maxcount));
//returns a new socket with listen enabled.
static qintptr_t VARGS Plug_Net_TCPListen(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int handle;
	int sock;
	struct sockaddr_qstorage address;
	int _true = 1;
	int alen;

	char *localip = VM_POINTER(arg[0]);
	unsigned short localport = VM_LONG(arg[1]);
	int maxcount = VM_LONG(arg[2]);

	netadr_t a;
	if (!localip)
		localip = "0.0.0.0";	//pass "[::]" for ipv6

	if (!NET_StringToAdr(localip, localport, &a))
		return -1;
	NetadrToSockadr(&a, &address);

	switch(((struct sockaddr*)&address)->sa_family)
	{
	case AF_INET:
		alen = sizeof(struct sockaddr_in);
		break;
#ifdef IPPROTO_IPV6
	case AF_INET6:
		alen = sizeof(struct sockaddr_in6);
		break;
#endif
	default:
		return -2;
	}


	if ((sock = socket(((struct sockaddr*)&address)->sa_family, SOCK_STREAM, 0)) == -1)
	{
		Con_Printf("Failed to create socket\n");
		return -2;
	}
	if (ioctlsocket (sock, FIONBIO, (u_long *)&_true) == -1)
	{
		closesocket(sock);
		return -2;
	}
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&_true, sizeof(_true));

	if (bind (sock, (void *)&address, alen) == -1)
	{
		closesocket(sock);
		return -2;
	}
	if (listen (sock, maxcount) == -1)
	{
		closesocket(sock);
		return -2;
	}

	handle = Plug_NewStreamHandle(STREAM_SOCKET);
	pluginstreamarray[handle].socket = sock;

	return handle;

}
static qintptr_t VARGS Plug_Net_Accept(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int handle = VM_LONG(arg[0]);
	struct sockaddr_in address;
	int addrlen;
	int sock;
	int _true = 1;
	char adr[MAX_ADR_SIZE];

	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug || pluginstreamarray[handle].type != STREAM_SOCKET)
		return -2;
	sock = pluginstreamarray[handle].socket;

	if (sock < 0)
		return -1;

	addrlen = sizeof(address);
	sock = accept(sock, (struct sockaddr *)&address, &addrlen);
	if (sock < 0)
		return -1;

	if (ioctlsocket (sock, FIONBIO, (u_long *)&_true) == -1)	//now make it non blocking.
	{
		closesocket(sock);
		return -1;
	}

	if (arg[2] && !VM_OOB(arg[1], arg[2]))
	{
		netadr_t a;
		char *s;
		SockadrToNetadr((struct sockaddr_qstorage *)&address, &a);
		s = NET_AdrToString(adr, sizeof(adr), &a);
		Q_strncpyz(VM_POINTER(arg[1]), s, addrlen);
	}

	handle = Plug_NewStreamHandle(STREAM_SOCKET);
	pluginstreamarray[handle].socket = sock;

	return handle;
}
//EBUILTIN(int, NET_TCPConnect, (char *ip, int port));
qintptr_t VARGS Plug_Net_TCPConnect(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	char *remoteip = VM_POINTER(arg[0]);
	unsigned short remoteport = VM_LONG(arg[1]);

	int handle;
	vfsfile_t *stream = FS_OpenTCP(remoteip, remoteport);
	if (!stream)
		return -1;
	handle = Plug_NewStreamHandle(STREAM_VFS);
	pluginstreamarray[handle].vfs = stream;
	return handle;

}


void Plug_Net_Close_Internal(int handle);
vfsfile_t *FS_OpenSSL(const char *hostname, vfsfile_t *source, qboolean server);
#ifdef HAVE_SSL
qintptr_t VARGS Plug_Net_SetTLSClient(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	pluginstream_t *stream;
	int handle = VM_LONG(arg[0]);
	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug)
	{
		Con_Printf("Plug_Net_SetTLSClient: socket does not belong to you (or is invalid)\n");
		return -2;
	}
	stream = &pluginstreamarray[handle];
	if (stream->type != STREAM_VFS)
	{	//not a socket - invalid
		Con_Printf("Plug_Net_SetTLSClient: Not a socket handle\n");
		return -2;
	}

	stream->vfs = FS_OpenSSL(VM_POINTER(arg[1]), stream->vfs, false);
	if (!stream->vfs)
	{
		Plug_Net_Close_Internal(handle);
		return -1;
	}
	return 0;
}
#endif
#endif

qintptr_t VARGS Plug_FS_Open(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	//modes:
	//1: read
	//2: write

	//char *name, int *handle, int mode

	//return value is length of the file.

	int handle;
	int *ret;
	//char *data;
	char *mode;
	vfsfile_t *f;
	char *fname = VM_POINTER(arg[0]);

	if (VM_OOB(arg[1], sizeof(int)))
		return -2;
	ret = VM_POINTER(arg[1]);

	switch(arg[2])
	{
	case 1:
		mode = "rb";
		break;
	case 2:
		mode = "wb";
		break;
	default:
		return -2;
	}
	if (!strcmp(fname, "**plugconfig"))
		f = FS_OpenVFS(va("%s.cfg", currentplug->name), mode, FS_ROOT);
	else if (arg[2] == 2)
		f = FS_OpenVFS(fname, mode, FS_GAMEONLY);
	else
		f = FS_OpenVFS(fname, mode, FS_GAME);
	if (!f)
		return -1;
	handle = Plug_NewStreamHandle(STREAM_VFS);
	pluginstreamarray[handle].vfs = f;
	*ret = handle;
	return VFS_GETLEN(pluginstreamarray[handle].vfs);
}
qintptr_t VARGS Plug_FS_Seek(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int handle = arg[0];
	unsigned int low = arg[1], high = arg[2];
	pluginstream_t *stream;

	stream = &pluginstreamarray[handle];
	if (stream->type != STREAM_VFS)
		return -1;
	VFS_SEEK(stream->vfs, low | ((unsigned long long)high<<32));
	return VFS_TELL(stream->vfs);
}

qintptr_t VARGS Plug_memset(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *p = VM_POINTER(arg[0]);

	if (VM_OOB(arg[0], arg[2]))
		return false;

	if (p)
		memset(p, VM_LONG(arg[1]), VM_LONG(arg[2]));

	return arg[0];
}
qintptr_t VARGS Plug_memcpy(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *p1 = VM_POINTER(arg[0]);
	void *p2 = VM_POINTER(arg[1]);

	if (VM_OOB(arg[0], arg[2]))
		return false;

	if (VM_OOB(arg[1], arg[2]))
		return false;

	if (p1 && p2)
		memcpy(p1, p2, VM_LONG(arg[2]));

	return arg[0];
}
qintptr_t VARGS Plug_memmove(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *p1 = VM_POINTER(arg[0]);
	void *p2 = VM_POINTER(arg[1]);

	if (VM_OOB(arg[0], arg[2]))
		return false;

	if (VM_OOB(arg[1], arg[2]))
		return false;

	if (p1 && p2)
		memmove(p1, p2, VM_LONG(arg[2]));

	return arg[0];
}

qintptr_t VARGS Plug_sqrt(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int ret;
	VM_FLOAT(ret) = sqrt(VM_FLOAT(arg[0]));
	return ret;
}
qintptr_t VARGS Plug_sin(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int ret;
	VM_FLOAT(ret) = sin(VM_FLOAT(arg[0]));
	return ret;
}
qintptr_t VARGS Plug_cos(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int ret;
	VM_FLOAT(ret) = cos(VM_FLOAT(arg[0]));
	return ret;
}
qintptr_t VARGS Plug_atan2(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int ret;
	VM_FLOAT(ret) = atan2(VM_FLOAT(arg[0]), VM_FLOAT(arg[1]));
	return ret;
}

void Plug_Net_Close_Internal(int handle)
{
	switch(pluginstreamarray[handle].type)
	{
	case STREAM_NONE:
		break;
	case STREAM_VFS:
		if (pluginstreamarray[handle].vfs)
			VFS_CLOSE(pluginstreamarray[handle].vfs);
		pluginstreamarray[handle].vfs = NULL;
		break;
	case STREAM_SOCKET:
#ifndef NACL
		closesocket(pluginstreamarray[handle].socket);
#endif
		break;
	}

	pluginstreamarray[handle].plugin = NULL;
}
qintptr_t VARGS Plug_Net_Recv(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int read;
	int handle = VM_LONG(arg[0]);
	void *dest = VM_POINTER(arg[1]);
	int destlen = VM_LONG(arg[2]);

	if (VM_OOB(arg[1], arg[2]))
		return -2;

	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug)
		return -2;
	switch(pluginstreamarray[handle].type)
	{
#ifndef NACL
	case STREAM_SOCKET:
		read = recv(pluginstreamarray[handle].socket, dest, destlen, 0);
		if (read < 0)
		{
			if (qerrno == EWOULDBLOCK)
				return -1;
			else
				return -2;
		}
		else if (read == 0)
			return -2;	//closed by remote connection.
		return read;
#endif

	case STREAM_VFS:
		return VFS_READ(pluginstreamarray[handle].vfs, dest, destlen);
	default:
		return -2;
	}
}
qintptr_t VARGS Plug_Net_Send(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int written;
	int handle = VM_LONG(arg[0]);
	void *src = VM_POINTER(arg[1]);
	int srclen = VM_LONG(arg[2]);
	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug)
		return -2;
	switch(pluginstreamarray[handle].type)
	{
#ifndef NACL
	case STREAM_SOCKET:
		written = send(pluginstreamarray[handle].socket, src, srclen, 0);
		if (written < 0)
		{
			if (qerrno == EWOULDBLOCK)
				return -1;
			else
				return -2;
		}
		else if (written == 0)
			return -2;	//closed by remote connection.
		return written;
#endif

	case STREAM_VFS:
		return VFS_WRITE(pluginstreamarray[handle].vfs, src, srclen);

	default:
		return -2;
	}
}
qintptr_t VARGS Plug_Net_SendTo(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int written;
	int handle = VM_LONG(arg[0]);
	void *src = VM_POINTER(arg[1]);
	int srclen = VM_LONG(arg[2]);

	netadr_t *address = VM_POINTER(arg[3]);


	struct sockaddr_qstorage sockaddr;
	if (handle == -1)
	{
		NET_SendPacket(NS_CLIENT, srclen, src, address);
		return srclen;
	}

	NetadrToSockadr(address, &sockaddr);

	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug)
		return -2;
	switch(pluginstreamarray[handle].type)
	{
#ifndef NACL
	case STREAM_SOCKET:
		written = sendto(pluginstreamarray[handle].socket, src, srclen, 0, (struct sockaddr*)&sockaddr, sizeof(sockaddr));
		if (written < 0)
		{
			if (qerrno == EWOULDBLOCK)
				return -1;
			else
				return -2;
		}
		else if (written == 0)
			return -2;	//closed by remote connection.
		return written;
#endif
	default:
		return -2;
	}
}
qintptr_t VARGS Plug_Net_Close(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	int handle = VM_LONG(arg[0]);
	if (handle < 0 || handle >= pluginstreamarraylen || pluginstreamarray[handle].plugin != currentplug)
		return -2;

	Plug_Net_Close_Internal(handle);
	return 0;
}

qintptr_t VARGS Plug_ReadInputBuffer(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *buffer = VM_POINTER(arg[0]);
	int bufferlen = VM_LONG(arg[1]);
	if (bufferlen > currentplug->inputbytes)
		bufferlen = currentplug->inputbytes;
	memcpy(buffer, currentplug->inputptr, currentplug->inputbytes);
	return bufferlen;
}
qintptr_t VARGS Plug_UpdateInputBuffer(void *offset, quintptr_t mask, const qintptr_t *arg)
{
	void *buffer = VM_POINTER(arg[0]);
	int bufferlen = VM_LONG(arg[1]);
	if (bufferlen > currentplug->inputbytes)
		bufferlen = currentplug->inputbytes;
	memcpy(currentplug->inputptr, buffer, currentplug->inputbytes);
	return bufferlen;
}

void Plug_CloseAll_f(void);
void Plug_List_f(void);
void Plug_Close_f(void);
void Plug_Load_f(void)
{
	char *plugin;
	plugin = Cmd_Argv(1);
	if (!*plugin)
	{
		Con_Printf("Loads a plugin\n");
		Con_Printf("plug_load [pluginpath]\n");
		Con_Printf("example pluginpath: blah\n");
		Con_Printf("will load fteplug_blah"ARCH_CPU_POSTFIX ARCH_DL_POSTFIX" or $gamedir/plugins/blah.qvm\n");
		return;
	}
	if (!Plug_Load(plugin, PLUG_EITHER))
	{
		if (!Plug_Load(va("plugins/%s", plugin), PLUG_QVM))
			Con_Printf("Couldn't load plugin %s\n", Cmd_Argv(1));
	}
}
/*
static qintptr_t Test_SysCalls_Ex(void *offset, quintptr_t mask, int fn, qintptr_t *arg)
{
	switch(fn)
	{
	case 1:
		Con_Printf("%s", VM_POINTER(arg[0]));
		break;
	default:
		Con_Printf("Can't handle %i\n", fn);
	}
	return 0;
}
static int EXPORT_FN Test_SysCalls(int arg, ...)
{
	return 0;
}
void VM_Test_f(void)
{
	vm_t *vm;
	vm = VM_Create(NULL, "vm/test", com_nogamedirnativecode.ival?NULL:Test_SysCalls, Test_SysCalls_Ex);
	if (vm)
	{
		VM_Call(vm, 0, "");
		VM_Destroy(vm);
	}
}*/

void Plug_Initialise(qboolean fromgamedir)
{
	char nat[MAX_OSPATH];

	if (!numplugbuiltins)
	{
		Cvar_Register(&plug_sbar, "plugins");
		Cvar_Register(&plug_loaddefault, "plugins");
		Cmd_AddCommand("plug_closeall", Plug_CloseAll_f);
		Cmd_AddCommand("plug_close", Plug_Close_f);
		Cmd_AddCommand("plug_load", Plug_Load_f);
		Cmd_AddCommand("plug_list", Plug_List_f);

		Plug_RegisterBuiltin("Plug_GetNativePointer",	Plug_GetNativePointer, 0);//plugin wishes to find a builtin number.
		Plug_RegisterBuiltin("Plug_GetEngineFunction",	Plug_GetBuiltin, 0);//plugin wishes to find a builtin number.
		Plug_RegisterBuiltin("Plug_ExportToEngine",		Plug_ExportToEngine, 0);	//plugin has a call back that we might be interested in.
		Plug_RegisterBuiltin("Plug_ExportNative",		Plug_ExportNative, PLUG_BIF_DLLONLY);
		Plug_RegisterBuiltin("Plug_GetPluginName",		Plug_GetPluginName, 0);
		Plug_RegisterBuiltin("Con_Print",				Plug_Con_Print, 0);	//printf is not possible - qvm floats are never doubles, vararg floats in a cdecl call are always converted to doubles.
		Plug_RegisterBuiltin("Sys_Error",				Plug_Sys_Error, 0);
		Plug_RegisterBuiltin("Sys_Milliseconds",		Plug_Sys_Milliseconds, 0);
		Plug_RegisterBuiltin("Com_Error",				Plug_Sys_Error, 0);	//make zquake programmers happy.

		Plug_RegisterBuiltin("Cmd_AddCommand",			Plug_Cmd_AddCommand, 0);
		Plug_RegisterBuiltin("Cmd_Args",				Plug_Cmd_Args, 0);
		Plug_RegisterBuiltin("Cmd_Argc",				Plug_Cmd_Argc, 0);
		Plug_RegisterBuiltin("Cmd_Argv",				Plug_Cmd_Argv, 0);
		Plug_RegisterBuiltin("Cmd_AddText",				Plug_Cmd_AddText, 0);

		Plug_RegisterBuiltin("Cvar_Register",			Plug_Cvar_Register, 0);
		Plug_RegisterBuiltin("Cvar_Update",				Plug_Cvar_Update, 0);
		Plug_RegisterBuiltin("Cvar_SetString",			Plug_Cvar_SetString, 0);
		Plug_RegisterBuiltin("Cvar_SetFloat",			Plug_Cvar_SetFloat, 0);
		Plug_RegisterBuiltin("Cvar_GetString",			Plug_Cvar_GetString, 0);
		Plug_RegisterBuiltin("Cvar_GetFloat",			Plug_Cvar_GetFloat, 0);

#ifndef NACL
		Plug_RegisterBuiltin("Net_TCPListen",			Plug_Net_TCPListen, 0);
		Plug_RegisterBuiltin("Net_Accept",				Plug_Net_Accept, 0);
		Plug_RegisterBuiltin("Net_TCPConnect",			Plug_Net_TCPConnect, 0);
#ifdef HAVE_SSL
		Plug_RegisterBuiltin("Net_SetTLSClient",		Plug_Net_SetTLSClient, 0);
#endif
		Plug_RegisterBuiltin("Net_Recv",				Plug_Net_Recv, 0);
		Plug_RegisterBuiltin("Net_Send",				Plug_Net_Send, 0);
		Plug_RegisterBuiltin("Net_SendTo",				Plug_Net_SendTo, 0);
		Plug_RegisterBuiltin("Net_Close",				Plug_Net_Close, 0);
#endif

		Plug_RegisterBuiltin("FS_Open",					Plug_FS_Open, 0);
		Plug_RegisterBuiltin("FS_Read",					Plug_Net_Recv, 0);
		Plug_RegisterBuiltin("FS_Write",				Plug_Net_Send, 0);
		Plug_RegisterBuiltin("FS_Close",				Plug_Net_Close, 0);
		Plug_RegisterBuiltin("FS_Seek",					Plug_FS_Seek, 0);


		Plug_RegisterBuiltin("memset",					Plug_memset, 0);
		Plug_RegisterBuiltin("memcpy",					Plug_memcpy, 0);
		Plug_RegisterBuiltin("memmove",					Plug_memmove, 0);
		Plug_RegisterBuiltin("sqrt",					Plug_sqrt, 0);
		Plug_RegisterBuiltin("sin",						Plug_sin, 0);
		Plug_RegisterBuiltin("cos",						Plug_cos, 0);
		Plug_RegisterBuiltin("atan2",					Plug_atan2, 0);

		Plug_RegisterBuiltin("ReadInputBuffer",			Plug_ReadInputBuffer, 0);
		Plug_RegisterBuiltin("UpdateInputBuffer",		Plug_UpdateInputBuffer, 0);

		Plug_Client_Init();
	}

	if (!fromgamedir)
	{
		FS_NativePath("", FS_BINARYPATH, nat, sizeof(nat));
		Con_DPrintf("Loading plugins from \"%s\"\n", nat);
		Sys_EnumerateFiles(nat, "fteplug_*" ARCH_CPU_POSTFIX ARCH_DL_POSTFIX, Plug_EnumeratedRoot, NULL, NULL);
	}
	if (fromgamedir)
	{
		if (plug_loaddefault.value)
		{
			COM_EnumerateFiles("plugins/*.qvm",		Plug_Emumerated, ".qvm");
		}
	}
}

void Plug_Tick(void)
{
	plugin_t *oldplug = currentplug;
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->tick)
		{
			VM_Call(currentplug->vm, currentplug->tick, (int)(realtime*1000));
		}
	}
	currentplug = oldplug;
}

#ifndef SERVERONLY
void Plug_ResChanged(void)
{
	plugin_t *oldplug = currentplug;
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->reschange)
			VM_Call(currentplug->vm, currentplug->reschange, vid.width, vid.height);
	}
	currentplug = oldplug;
}
#endif

qboolean Plugin_ExecuteString(void)
{
	plugin_t *oldplug = currentplug;
	if (Cmd_Argc()>0)
	{
		for (currentplug = plugs; currentplug; currentplug = currentplug->next)
		{
			if (currentplug->executestring)
			{
				if (VM_Call(currentplug->vm, currentplug->executestring, 0))
				{
					currentplug = oldplug;
					return true;
				}
			}
		}
	}
	currentplug = oldplug;
	return false;
}

#ifndef SERVERONLY
qboolean Plug_ConsoleLinkMouseOver(float x, float y, char *text, char *info)
{
	qboolean result = false;
	plugin_t *oldplug = currentplug;
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->consolelinkmouseover)
		{
			char buffer[2048];
			Q_strncpyz(buffer, va("\"%s\" \"%s\"", text, info), sizeof(buffer));
			Cmd_TokenizeString(buffer, false, false);
			result = VM_Call(currentplug->vm, currentplug->consolelinkmouseover, *(int*)&(x), *(int*)&(y));
			if (result)
				break;
		}
	}
	currentplug = oldplug;
	return result;
}
qboolean Plug_ConsoleLink(char *text, char *info)
{
	qboolean result = false;
	plugin_t *oldplug = currentplug;
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->consolelink)
		{
			char buffer[2048];
			Q_strncpyz(buffer, va("\"%s\" \"%s\"", text, info), sizeof(buffer));
			Cmd_TokenizeString(buffer, false, false);
			result = VM_Call(currentplug->vm, currentplug->consolelink);
			if (result)
				break;
		}
	}
	currentplug = oldplug;
	return result;
}

void Plug_SubConsoleCommand(console_t *con, char *line)
{
	char buffer[2048];
	plugin_t *oldplug = currentplug;	//shouldn't really be needed, but oh well
	currentplug = con->userdata;

	Q_strncpyz(buffer, va("\"%s\" %s", con->name, line), sizeof(buffer));
	Cmd_TokenizeString(buffer, false, false);
	VM_Call(currentplug->vm, currentplug->conexecutecommand, 0);
	currentplug = oldplug;
}

void Plug_SpellCheckMaskedText(unsigned int *maskedstring, int maskedchars, int x, int y, int cs, int firstc, int charlimit)
{
	plugin_t *oldplug = currentplug;
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->spellcheckmaskedtext)
		{
			currentplug->inputptr = maskedstring;
			currentplug->inputbytes = sizeof(*maskedstring)*maskedchars;
			VM_Call(currentplug->vm, currentplug->spellcheckmaskedtext, x, y, cs, firstc, charlimit);
			currentplug->inputptr = NULL;
			currentplug->inputbytes = 0;
		}
	}
	currentplug = oldplug;
}
#endif

#ifndef SERVERONLY
qboolean Plug_Menu_Event(int eventtype, int param)	//eventtype = draw/keydown/keyup, param = time/key
{
	plugin_t *oc=currentplug;
	qboolean ret;

	if (!menuplug)
		return false;

	currentplug = menuplug;
	ret = VM_Call(menuplug->vm, menuplug->menufunction, eventtype, param, (int)mousecursor_x, (int)mousecursor_y);
	currentplug=oc;
	return ret;
}
#endif
#ifndef SERVERONLY
int Plug_ConnectionlessClientPacket(char *buffer, int size)
{
	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->connectionlessclientpacket)
		{
			switch (VM_Call(currentplug->vm, currentplug->connectionlessclientpacket, buffer, size, &net_from))
			{
			case 0:
				continue;	//wasn't handled
			case 1:
				currentplug = NULL;	//was handled with no apparent result
				return true;
			case 2:
#ifndef SERVERONLY
				cls.protocol = CP_PLUGIN;	//woo, the plugin wants to connect to them!
				protocolclientplugin = currentplug;
#endif
				currentplug = NULL;
				return true;
			}
		}
	}
	return false;
}
#endif
#ifndef SERVERONLY
void Plug_SBar(playerview_t *pv)
{
	extern qboolean sb_showscores, sb_showteamscores;

	plugin_t *oc=currentplug;
	int ret;

	if (!Sbar_ShouldDraw())
		return;

	ret = 0;
	if (!plug_sbar.ival || cl.splitclients > 1)
		currentplug = NULL;
	else
	{
		for (currentplug = plugs; currentplug; currentplug = currentplug->next)
		{
			if (currentplug->sbarlevel[0])
			{
				//if you don't use splitscreen, use a full videosize rect.
				R2D_ImageColours(1, 1, 1, 1); // ensure menu colors are reset
				ret |= VM_Call(currentplug->vm, currentplug->sbarlevel[0], pv-cl.playerview, r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height, sb_showscores+sb_showteamscores*2);
				break;
			}
		}
	}
	if (!(ret & 1))
	{
		Sbar_Draw(pv);
	}

	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->sbarlevel[1])
		{
			R2D_ImageColours(1, 1, 1, 1); // ensure menu colors are reset
			ret |= VM_Call(currentplug->vm, currentplug->sbarlevel[1], pv-cl.playerview, r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height, sb_showscores+sb_showteamscores*2);
		}
	}

	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->sbarlevel[2])
		{
			R2D_ImageColours(1, 1, 1, 1); // ensure menu colors are reset
			ret |= VM_Call(currentplug->vm, currentplug->sbarlevel[2], pv-cl.playerview, r_refdef.vrect.x, r_refdef.vrect.y, r_refdef.vrect.width, r_refdef.vrect.height, sb_showscores+sb_showteamscores*2);
		}
	}

	if (!(ret & 2) && pv == cl.playerview)
	{
		Sbar_DrawScoreboard();
	}


	currentplug = oc;
}
#endif

qboolean Plug_ServerMessage(char *buffer, int messagelevel)
{
	qboolean ret = true;

	Cmd_TokenizeString(buffer, false, false);
	Cmd_Args_Set(buffer);

	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->svmsgfunction)
		{
			ret &= VM_Call(currentplug->vm, currentplug->svmsgfunction, messagelevel);
		}
	}

	Cmd_Args_Set(NULL);

	return ret; // true to display message, false to supress
}

qboolean Plug_ChatMessage(char *buffer, int talkernum, int tpflags)
{
	qboolean ret = true;

	Cmd_TokenizeString(buffer, false, false);
	Cmd_Args_Set(buffer);

	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->chatmsgfunction)
		{
			ret &= VM_Call(currentplug->vm, currentplug->chatmsgfunction, talkernum, tpflags);
		}
	}

	Cmd_Args_Set(NULL);

	return ret; // true to display message, false to supress
}

qboolean Plug_CenterPrintMessage(char *buffer, int clientnum)
{
	qboolean ret = true;

	Cmd_TokenizeString(buffer, false, false);
	Cmd_Args_Set(buffer);

	for (currentplug = plugs; currentplug; currentplug = currentplug->next)
	{
		if (currentplug->centerprintfunction)
		{
			ret &= VM_Call(currentplug->vm, currentplug->centerprintfunction, clientnum);
		}
	}

	Cmd_Args_Set(NULL);

	return ret; // true to display message, false to supress
}

void Plug_Close(plugin_t *plug)
{
	int i;
	if (plug->blockcloses)
	{
		Con_Printf("Plugin %s provides driver features, and cannot safely be unloaded\n", plug->name);
		return;
	}
	if (plugs == plug)
		plugs = plug->next;
	else
	{
		plugin_t *prev;
		for (prev = plugs; prev; prev = prev->next)
		{
			if (prev->next == plug)
				break;
		}
		if (!prev)
			Sys_Error("Plug_Close: not linked\n");
		prev->next = plug->next;
	}

	Con_Printf("Closing plugin %s\n", plug->name);

	//ensure any active contexts provided by the plugin are closed (stuff with destroy callbacks)
#if defined(PLUGINS) && !defined(NOMEDIA) && !defined(SERVERONLY)
	Media_UnregisterDecoder(plug, NULL);
	Media_UnregisterEncoder(plug, NULL);
#endif
	FS_UnRegisterFileSystemModule(plug);

	//tell the plugin that everything is closed and that it should free up any lingering memory/stuff
	//it is still allowed to create/have open files.
	if (plug->shutdown)
	{
		plugin_t *cp = currentplug;
		currentplug = plug;
		VM_Call(plug->vm, plug->shutdown);
		currentplug = cp;
	}

	VM_Destroy(plug->vm);

	//make sure anything else that was left is unlinked (stuff without destroy callbacks).
	for (i = 0; i < pluginstreamarraylen; i++)
	{
		if (pluginstreamarray[i].plugin == plug)
		{
			Plug_Net_Close_Internal(i);
		}
	}

	Plug_FreeConCommands(plug);

	Plug_Client_Close(plug);
	Z_Free(plug);

	if (currentplug == plug)
		currentplug = NULL;
}

void Plug_Close_f(void)
{
	plugin_t *plug;
	char *name = Cmd_Argv(1);
	if (Cmd_Argc()<2)
	{
		Con_Printf("Close which plugin?\n");
		return;
	}

	if (currentplug)
		Sys_Error("Plug_CloseAll_f called inside a plugin!\n");

	for (plug = plugs; plug; plug = plug->next)
	{
		if (!strcmp(plug->name, name))
		{
			Plug_Close(plug);
			return;
		}
	}

	name = va("plugins/%s", name);
	for (plug = plugs; plug; plug = plug->next)
	{
		if (!strcmp(plug->name, name))
		{
			Plug_Close(plug);
			return;
		}
	}
	Con_Printf("Plugin %s does not appear to be loaded\n", Cmd_Argv(1));
}

void Plug_CloseAll_f(void)
{
	plugin_t *p;
	if (currentplug)
		Sys_Error("Plug_CloseAll_f called inside a plugin!\n");
	while(plugs)
	{
		p = plugs;
		while (p->blockcloses)
		{
			p = p->next;
			if (!p)
				return;
		}
		Plug_Close(p);
	}
}

void Plug_List_f(void)
{
	plugin_t *plug;
	for (plug = plugs; plug; plug = plug->next)
	{
		VM_PrintInfo(plug->vm);
	}
}

void Plug_Shutdown(qboolean preliminary)
{
	plugin_t **p;
	if (preliminary)
	{
		//close the non-block-closes plugins first, before most of the rest of the subsystems are down
		for (p = &plugs; *p; )
		{
			if ((*p)->blockcloses)
				p = &(*p)->next;
			else
				Plug_Close(plugs);
		}
	}
	else
	{
		//now that our various handles etc are closed, its safe to terminate the various driver plugins.
		while(plugs)
		{
			plugs->blockcloses = 0;
			Plug_Close(plugs);
		}

		BZ_Free(pluginstreamarray);
		pluginstreamarray = NULL;
		pluginstreamarraylen = 0;

		numplugbuiltins = 0;
		BZ_Free(plugbuiltins);
		plugbuiltins = NULL;

		plugincvararraylen = 0;
		BZ_Free(plugincvararray);
		plugincvararray = NULL;

		plugincommandarraylen = 0;
		BZ_Free(plugincommandarray);
		plugincommandarray = NULL;

#ifndef SERVERONLY
		Plug_Client_Shutdown();
#endif
	}
}

#endif