#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <share.h>
#include <string.h>
#include <map>
#include <string>
#include <array>
#include "MinHook.h"

// UC 1.17.0.66, x86. All engine calls below run on the game's UI thread.
// R is the roof-system interface (vtable 01258538); B=R-8 is its concrete
// object; S=R+4 is the ROOF serialization interface. These are distinct ABIs.
using U=uint32_t;
using P=unsigned char*;
static HMODULE module;
static bool enabled=false,paintMode=false,changingMaterial=false;
static UINT_PTR pickerTimer=0;
static U selectedPattern=0,generation=1;
static P loadingRoot=nullptr,currentRoot=nullptr;
static thread_local U builderRoof=0;
static std::map<U,U> patterns;

// Runtime diagnostic sidecar removed; native ROOF save/load remains intact.
static void Log(const char*,...) {}
template<class T> static T Fn(U address){return reinterpret_cast<T>(address);}
template<class T> static T V(void* object,U offset){return reinterpret_cast<T>((*reinterpret_cast<U**>(object))[offset/4]);}
static U& Field(void* object,U offset){return *reinterpret_cast<U*>(static_cast<P>(object)+offset);}
static P GetRoot(){
    auto world=Fn<void*(__cdecl*)()>(0x799AB9)();
    return world?static_cast<P>(V<void*(__thiscall*)(void*)>(world,0x90)(world)):nullptr;
}
static void AdoptRoot(P root){
    if(root==currentRoot)return;
    currentRoot=root;patterns.clear();++generation;paintMode=false;
}
static U Pattern(U id){auto it=patterns.find(id);return it==patterns.end()?Field(currentRoot,0x30):it->second;}
static void* Descriptor(U id){return Fn<void*(__thiscall*)(void*,U)>(0xC4F000)(currentRoot,id);}

static const char* materialNames[]={"RoofTop","RoofUnder","RoofTrim","RoofEdges","RoofMaterialStrips"};
struct Materials{std::array<std::string,5> names;};
static std::map<U,Materials> materialCache;
static void* MaterialManager(){return Fn<void*(__cdecl*)()>(0xE745A0)();}
static int FindMaterial(void* manager,const char* name){return V<int(__thiscall*)(void*,const char*)>(manager,0x4C)(manager,name);}
static void* CatalogEntry(U pattern){
    void* framework=Fn<void*(__cdecl*)()>(0x799A0D)();
    if(!framework)return nullptr;
    void* catalog=V<void*(__thiscall*)(void*)>(framework,0x40)(framework);
    return catalog?V<void*(__thiscall*)(void*,U,bool)>(catalog,0x10C)(catalog,pattern,true):nullptr;
}
static Materials* GetMaterials(U pattern){
    auto found=materialCache.find(pattern);if(found!=materialCache.end())return &found->second;
    void* entry=CatalogEntry(pattern);if(!entry)return nullptr;
    void* manager=MaterialManager();Materials result;
    const U getters[]={0x20,0x2C,0x34,0x30,0x20};
    // Clone the current material definitions to preserve installed shaders.
    // The aliases, their textures, and each model binding are independent.
    for(unsigned i=0;i<5;i++){
        char alias[96];sprintf_s(alias,"IndividualRoof_%08X_%s",pattern,materialNames[i]);result.names[i]=alias;
        {
            // Lookup creates an empty material when a name is absent. Always
            // register a real clone on this cache's first use; an index alone
            // cannot distinguish a valid material from that placeholder.
            int index=FindMaterial(manager,materialNames[i]);if(index<0)return nullptr;
            U descriptor[5]={};V<void*(__thiscall*)(void*,void*,int)>(manager,0x84)(manager,descriptor,index);
            void* source=reinterpret_cast<void*>(descriptor[1]);if(!source)return nullptr;
            void* copy=V<void*(__thiscall*)(void*)>(source,0x1C)(source);if(!copy)return nullptr;
            V<void(__thiscall*)(void*,const char*)>(copy,0x28)(copy,alias);
            int registered=V<int(__thiscall*)(void*,void*,U)>(manager,0x5C)(manager,copy,0);
            if(registered<0||FindMaterial(manager,alias)!=registered)return nullptr;
        }
        const char* texture=V<const char*(__thiscall*)(void*)>(entry,getters[i])(entry);
        const char* bump=i==0?V<const char*(__thiscall*)(void*)>(entry,0x24)(entry):nullptr;
        bool useBump=i==0?V<bool(__thiscall*)(void*)>(entry,0x28)(entry):false;
        Fn<void(__stdcall*)(const char*,const char*,const char*,bool)>(0x4AFF37)(alias,texture,bump,useBump);
    }
    Log("Material set %08X ready\n",pattern);
    return &materialCache.emplace(pattern,std::move(result)).first->second;
}
using BuildFn=void(__thiscall*)(void*);
static BuildFn buildOriginal[2];
static void __fastcall Build0(void* self,void*){U old=builderRoof;builderRoof=Field(self,0x20);buildOriginal[0](self);builderRoof=old;}
static void __fastcall Build1(void* self,void*){U old=builderRoof;builderRoof=Field(self,0x20);buildOriginal[1](self);builderRoof=old;}
using RenderCreateFn=void(__thiscall*)(void*,U,void*,void*,U);
static RenderCreateFn renderCreateOriginal;
static void __fastcall RenderCreate(void* self,void*,U id,void* a,void* b,U type){
    U old=builderRoof;builderRoof=id;renderCreateOriginal(self,id,a,b,type);builderRoof=old;
}
using RefreshFn=bool(__thiscall*)(void*,U);
static RefreshFn refreshOriginal;
static bool __fastcall Refresh(void* self,void*,U id){
    U old=builderRoof;builderRoof=id;bool result=refreshOriginal(self,id);builderRoof=old;return result;
}
using BindFn=void(__thiscall*)(void*,const char*,const char*);
static BindFn bindOriginal;
static void __fastcall Bind(void* self,void*,const char* material,const char* subset){
    if(builderRoof&&currentRoot&&material&&!changingMaterial){
        for(unsigned i=0;i<5;i++)if(strcmp(material,materialNames[i])==0){
            U pattern=Pattern(builderRoof);
            if(pattern!=Field(currentRoot,0x30)){
                changingMaterial=true;Materials* set=GetMaterials(pattern);changingMaterial=false;
                if(set)material=set->names[i].c_str();
            }
            break;
        }
    }
    bindOriginal(self,material,subset);
}
static bool SetPattern(U id,U pattern){
    if(!currentRoot||!Descriptor(id)||!GetMaterials(pattern))return false;
    void* descriptor=Descriptor(id);U level=Field(descriptor,0x10);
    patterns[id]=pattern;
    Fn<bool(__thiscall*)(void*,int)>(0xC4FBF0)(currentRoot,static_cast<int>(level));
    // Match the native individual-pitch tool's build/lot invalidation.
    void* world=Fn<void*(__cdecl*)()>(0x4AE0DC)();
    if(world){void* build=V<void*(__thiscall*)(void*)>(world,0x9C)(world);
        if(build)V<void(__thiscall*)(void*,int)>(build,0x158)(build,2);}
    Log("Roof %u pattern %08X\n",id,pattern);return true;
}

// Store the pattern in the native per-roof ROOF record. No sidecar files.
using LoadFn=bool(__thiscall*)(void*,void*);
static LoadFn loadOriginal;
static bool __fastcall Load(void* self,void*,void* stream){
    P root=static_cast<P>(self)-4;currentRoot=root;patterns.clear();++generation;paintMode=false;
    P previous=loadingRoot;loadingRoot=root;bool ok=loadOriginal(self,stream);loadingRoot=previous;
    if(!ok)patterns.clear();Log("ROOF load %s: %zu patterns\n",ok?"ok":"failed",patterns.size());return ok;
}
using CreateFn=int(__thiscall*)(void*,void*,void*,U,float,U,int);
static CreateFn createOriginal;
static int __fastcall Create(void* self,void*,void* a,void* b,U type,float pitch,U catalog,int id){
    P root=static_cast<P>(self)+8;U pattern=Field(root,0x30);
    if(loadingRoot==root&&id>=0)patterns[static_cast<U>(id)]=pattern;
    int created=createOriginal(self,a,b,type,pitch,catalog,id);
    if(created>=0&&root==currentRoot&&(loadingRoot==root||patterns.find(created)==patterns.end()))patterns[created]=pattern;
    return created;
}
static U __cdecl SavePattern(P root,U id){
    if(root!=currentRoot)return Field(root,0x30);
    return Pattern(id);
}
static void* saveOriginal;
static U saveContinue=0xC508ED;
__declspec(naked) static void SavePatternHook(){
    __asm{
        pushfd
        pushad
        push dword ptr [edi+4]
        lea eax,[ebp-4]
        push eax
        call SavePattern
        add esp,8
        mov dword ptr [esp+28],eax
        popad
        popfd
        mov edx,dword ptr [esi]
        jmp dword ptr [saveContinue]
    }
}
using GlobalFn=void(__thiscall*)(void*,U);
static GlobalFn globalOriginal;
static void __fastcall Global(void* self,void*,U pattern){
    globalOriginal(self,pattern);
    if(currentRoot==self){for(auto& item:patterns)item.second=pattern;
        Log("Whole-lot roof pattern %08X\n",pattern);
        // Models with private aliases must return to the new whole-lot pattern.
        const U levels=(Field(self,0xC)-Field(self,8))/20;
        for(U level=0;level<levels;level+=3)Fn<bool(__thiscall*)(void*,int)>(0xC4FBF0)(self,static_cast<int>(level));
    }
}
using ShutdownFn=bool(__thiscall*)(void*);
static ShutdownFn shutdownOriginal;
static bool __fastcall Shutdown(void* self,void*){
    if(currentRoot==self){currentRoot=nullptr;patterns.clear();++generation;paintMode=false;}
    return shutdownOriginal(self);
}

// Use the game's own roof-pattern command object and undo stack. Its native
// geometry/pattern vector stays empty; our entry supplies per-roof state.
struct UndoEntry{P root;U generation,id,before,after;};
static std::map<void*,UndoEntry> undoEntries;
using UndoFn=void(__thiscall*)(void*);
static UndoFn undoOriginal,commandDestroyOriginal;
static void __fastcall Undo(void* self,void*){
    auto found=undoEntries.find(self);if(found==undoEntries.end()){undoOriginal(self);return;}
    const auto& entry=found->second;
    if(entry.root!=currentRoot||entry.generation!=generation)return;
    U& state=Field(self,0x58);
    if(state==2){if(SetPattern(entry.id,entry.before))state=1;}
    else if(state==1){if(SetPattern(entry.id,entry.after))state=2;}
}
static void __fastcall CommandDestroy(void* self,void*){undoEntries.erase(self);commandDestroyOriginal(self);}
static bool Paint(void* tool,U id){
    U before=Pattern(id);if(before==selectedPattern)return true;
    void* manager=reinterpret_cast<void*>(Field(tool,0x4C));if(!manager)return false;
    void* command=Fn<void*(__cdecl*)()>(0x4B0309)();if(!command)return false;
    V<int(__thiscall*)(void*)>(command,4)(command);
    if(!SetPattern(id,selectedPattern)){V<int(__thiscall*)(void*)>(command,8)(command);return false;}
    undoEntries[command]={currentRoot,generation,id,before,selectedPattern};Field(command,0x58)=2;
    V<void(__thiscall*)(void*,void*)>(manager,0x10)(manager,command);
    V<int(__thiscall*)(void*)>(command,8)(command);return true;
}
static bool ActivatePicker(){
    void* manager=Fn<void*(__cdecl*)()>(0x799A38)();if(!manager)return false;
    void* payload=Fn<void*(__cdecl*)(U)>(0xE5BC0B)(0x14);if(!payload)return false;
    payload=Fn<void*(__thiscall*)(void*)>(0x40A254)(payload);
    V<int(__thiscall*)(void*)>(payload,4)(payload);
    V<void(__thiscall*)(void*,float)>(payload,0x9C)(payload,0.5f);
    V<void(__thiscall*)(void*,U,void*)>(manager,0xC8)(manager,0x95BFCD91,payload);
    V<int(__thiscall*)(void*)>(payload,8)(payload);return true;
}
static void CALLBACK PickerTimer(HWND,UINT,UINT_PTR timer,DWORD){
    KillTimer(nullptr,timer);if(pickerTimer==timer)pickerTimer=0;
    if(enabled&&paintMode&&currentRoot)ActivatePicker();
}
using MoveFn=void(__thiscall*)(void*,int*);
static MoveFn moveOriginal;
static void __fastcall Move(void* self,void*,int* xy){
    if(!paintMode){moveOriginal(self,xy);return;}
    Fn<void(__thiscall*)(void*,int*)>(0x605183)(self,xy);
    V<void(__thiscall*)(void*,int,int)>(self,0xDC)(self,xy[0],xy[1]);
    if(!*(static_cast<P>(self)+0x58))return;
    Fn<void(__thiscall*)(void*)>(0x4C109A)(self);
    if(Field(self,0x5C)==7&&static_cast<int>(Field(self,0xB0))>0){
        Paint(self,Field(self,0xB0));
        *(static_cast<P>(self)+0x58)=0;
    }
}
using ChooseFn=bool(__thiscall*)(void*,U);
static ChooseFn chooseOriginal;
static bool __fastcall Choose(void* self,void*,U pattern){
    if(!paintMode)return chooseOriginal(self,pattern);
    if(!CatalogEntry(pattern))return false;
    selectedPattern=pattern;
    // The catalogue resets its old tool after this command returns. Activate
    // at the next message boundary, after that reset has finished.
    if(pickerTimer)KillTimer(nullptr,pickerTimer);
    pickerTimer=SetTimer(nullptr,0,1,PickerTimer);
    Log("Selected per-roof pattern %08X\n",pattern);
    // The catalogue's selection command must not enter the undo stack;
    // painting an actual roof creates the undoable command instead.
    return false;
}
using PanelFn=bool(__thiscall*)(void*,void*,void*);
static PanelFn panelOriginal;
static constexpr U buttonID=0xF0EEB970;
static void HighlightTool(void* panel){
    if(Field(panel,0x70)!=9)return; // Roof section only.
    void* view=static_cast<P>(panel)+8;
    void* window=V<void*(__thiscall*)(void*)>(view,0x3C)(view);
    if(!window)return;
    // Native helper takes (root window, child ID, selected); it acquires the
    // button interface and invalidates the view after changing its state.
    auto select=Fn<void(__stdcall*)(void*,U,bool)>(0x53917B);
    select(window,0xB930,!paintMode&&Field(panel,0x90)==13);
    select(window,buttonID,paintMode);
}
using UpdatePanelFn=void(__thiscall*)(void*,U,U,U);
static UpdatePanelFn updatePanelOriginal;
// Native 0053AD72 returns with RET 0Ch: all three stack arguments must
// be preserved, including the two catalogue/update flags.
static void __fastcall UpdatePanel(void* self,void*,U category,U arg2,U arg3){
    updatePanelOriginal(self,category,arg2,arg3);HighlightTool(self);
}

static bool __fastcall Panel(void* self,void*,void* source,void* message){
    U id=message?Field(message,8):0;
    if(id==buttonID){
        // Reuse the existing full pattern catalogue, including custom content.
        Field(message,8)=0xB930;bool result=panelOriginal(self,source,message);Field(message,8)=id;
        AdoptRoot(GetRoot());if(!currentRoot)return result;paintMode=true;selectedPattern=Field(currentRoot,0x30);
        HighlightTool(self);Log("Individual roof catalogue opened\n");return result;
    }
    if(id>=0xA000&&id<=0xC400)paintMode=false;
    return panelOriginal(self,source,message);
}

struct Hook{U address;void* detour;void** original;};
static Hook hooks[]={
    {0xA60600,reinterpret_cast<void*>(RenderCreate),reinterpret_cast<void**>(&renderCreateOriginal)},
    {0xA5EED0,reinterpret_cast<void*>(Refresh),reinterpret_cast<void**>(&refreshOriginal)},
    {0xB3B420,reinterpret_cast<void*>(Build0),reinterpret_cast<void**>(&buildOriginal[0])},
    {0xB40730,reinterpret_cast<void*>(Build1),reinterpret_cast<void**>(&buildOriginal[1])},
    {0xFA6EB0,reinterpret_cast<void*>(Bind),reinterpret_cast<void**>(&bindOriginal)},
    {0xC51B10,reinterpret_cast<void*>(Load),reinterpret_cast<void**>(&loadOriginal)},
    {0xC51290,reinterpret_cast<void*>(Create),reinterpret_cast<void**>(&createOriginal)},
    {0xC508E8,reinterpret_cast<void*>(SavePatternHook),&saveOriginal},
    {0xD29180,reinterpret_cast<void*>(Global),reinterpret_cast<void**>(&globalOriginal)},
    {0xC52490,reinterpret_cast<void*>(Shutdown),reinterpret_cast<void**>(&shutdownOriginal)},
    {0x4B00E7,reinterpret_cast<void*>(Undo),reinterpret_cast<void**>(&undoOriginal)},
    {0x4B01D4,reinterpret_cast<void*>(CommandDestroy),reinterpret_cast<void**>(&commandDestroyOriginal)},
    {0x4BA612,reinterpret_cast<void*>(Move),reinterpret_cast<void**>(&moveOriginal)},
    {0x4B03AB,reinterpret_cast<void*>(Choose),reinterpret_cast<void**>(&chooseOriginal)},
    {0x53CDB7,reinterpret_cast<void*>(Panel),reinterpret_cast<void**>(&panelOriginal)},
    {0x53AD72,reinterpret_cast<void*>(UpdatePanel),reinterpret_cast<void**>(&updatePanelOriginal)}
};
static bool Signature(U address,const char* bytes,size_t length){return memcmp(reinterpret_cast<void*>(address),bytes,length)==0;}
static bool Profile(){
    if(GetModuleHandleW(nullptr)!=reinterpret_cast<HMODULE>(0x400000))return false;
    return Signature(0xC51B10,"\x83\xEC\x4C\x56\x8B\x74\x24\x54",8)
        &&Signature(0xC508E8,"\x8B\x45\x2C\x8B\x16",5)
        &&Signature(0xFA6EB0,"\x6A\xFF\x68\xA8\x4F\x1B\x01",7)
        &&Signature(0x4BA612,"\x56\x57\x8B\x7C\x24\x0C",6)
        &&Signature(0xD29180,"\x8B\x44\x24\x04\x89\x41\x30",7)
        &&Field(reinterpret_cast<void*>(0x1258538),0x64)==0xC4F000;
}
extern "C" __declspec(dllexport) int __cdecl InitializeRoofTextures(){
    if(enabled)return 1;
    if(!Profile()){Log("Unsupported executable profile; no hooks installed.\n");return 0;}
    if(MH_Initialize()!=MH_OK){Log("MinHook initialization failed\n");return 0;}
    for(auto& hook:hooks){MH_STATUS status=MH_CreateHook(reinterpret_cast<void*>(hook.address),hook.detour,hook.original);
        if(status!=MH_OK){Log("Hook %08X create failed %d\n",hook.address,status);for(auto& rollback:hooks)MH_RemoveHook(reinterpret_cast<void*>(rollback.address));return 0;}}
    // Never enable hooks owned by another ASI's MinHook instance.
    for(auto& hook:hooks)MH_QueueEnableHook(reinterpret_cast<void*>(hook.address));
    if(MH_ApplyQueued()!=MH_OK){Log("Hook activation failed\n");return 0;}
    enabled=true;Log("UC individual roof textures initialized\n");return 1;
}
// Developer-only live-test entry points; the normal tool uses its catalogue button.
extern "C" __declspec(dllexport) int __cdecl TestSelectRoofPattern(U pattern){
    if(!enabled||!CatalogEntry(pattern))return 0;AdoptRoot(GetRoot());selectedPattern=pattern;paintMode=true;return ActivatePicker()?1:0;
}
extern "C" __declspec(dllexport) int __cdecl TestSetRoofPattern(U id,U pattern){
    if(!enabled)return 0;AdoptRoot(GetRoot());return SetPattern(id,pattern)?1:0;
}
extern "C" __declspec(dllexport) U __cdecl TestGetRoofPattern(U id){return currentRoot?Pattern(id):0;}
extern "C" __declspec(dllexport) int __cdecl StopRoofTextures(){
    if(!enabled)return 1;paintMode=false;
    if(pickerTimer){KillTimer(nullptr,pickerTimer);pickerTimer=0;}
    for(auto& hook:hooks)MH_QueueDisableHook(reinterpret_cast<void*>(hook.address));
    if(MH_ApplyQueued()!=MH_OK)return 0;enabled=false;return 1;
}
static DWORD WINAPI Start(void*){InitializeRoofTextures();return 0;}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH){module=instance;DisableThreadLibraryCalls(instance);
        HANDLE thread=CreateThread(nullptr,0,Start,nullptr,0,nullptr);if(thread)CloseHandle(thread);}
    return TRUE;
}
