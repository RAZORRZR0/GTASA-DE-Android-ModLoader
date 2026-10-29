#include <mod/amlmod.h>
#include <mod/logger.h>

MYMOD(net.gtamod.hello, Hello, 1.0, gtamod)
NEEDGAME(com.rockstargames.gtasa.de)

// CObject::SetIsStatic(bool): this + bool
DECL_HOOKv(SetIsStatic, void* self, bool bStatic)
{
    logger->Info("SetIsStatic(this=%p, static=%d)", self, (int)bStatic);
    SetIsStatic(self, bStatic);
}

extern "C" void OnModLoad()
{
    logger->SetTag("Hello Mod");

    void* hUE4 = aml->GetLibHandle("libUE4.so");
    if(!hUE4)
    {
        logger->Error("libUE4.so not loaded");
        return;
    }

    uintptr_t pGUObjectArray = aml->GetSym(hUE4, "GUObjectArray");
    uintptr_t pSetIsStatic = aml->GetSym(hUE4, "_ZN7CObject11SetIsStaticEb");
    logger->Info("GUObjectArray = 0x%lX", pGUObjectArray);
    logger->Info("CObject::SetIsStatic = 0x%lX", pSetIsStatic);

    if(pSetIsStatic)
    {
        HOOK(SetIsStatic, pSetIsStatic);
    }
}
