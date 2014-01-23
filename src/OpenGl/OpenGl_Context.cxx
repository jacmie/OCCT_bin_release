// Created on: 2012-01-26
// Created by: Kirill GAVRILOV
// Copyright (c) 2012-2014 OPEN CASCADE SAS
//
// This file is part of Open CASCADE Technology software library.
//
// This library is free software; you can redistribute it and / or modify it
// under the terms of the GNU Lesser General Public version 2.1 as published
// by the Free Software Foundation, with special exception defined in the file
// OCCT_LGPL_EXCEPTION.txt. Consult the file LICENSE_LGPL_21.txt included in OCCT
// distribution for complete text of the license and disclaimer of any warranty.
//
// Alternatively, this file may be used under the terms of Open CASCADE
// commercial license or contractual agreement.

#if defined(_WIN32)
  #include <windows.h>
#endif

#include <OpenGl_Context.hxx>

#include <OpenGl_ArbVBO.hxx>
#include <OpenGl_ArbTBO.hxx>
#include <OpenGl_ArbIns.hxx>
#include <OpenGl_ArbDbg.hxx>
#include <OpenGl_ExtFBO.hxx>
#include <OpenGl_ExtGS.hxx>
#include <OpenGl_GlCore20.hxx>
#include <OpenGl_ShaderManager.hxx>

#include <Message_Messenger.hxx>

#include <NCollection_Vector.hxx>

#include <Standard_ProgramError.hxx>

#if defined(_WIN32)
  //
#elif defined(__APPLE__) && !defined(MACOSX_USE_GLX)
  #include <dlfcn.h>
#else
  #include <GL/glx.h> // glXGetProcAddress()
#endif

// GL_NVX_gpu_memory_info
#ifndef GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
  enum
  {
    GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX         = 0x9047,
    GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX   = 0x9048,
    GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX = 0x9049,
    GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX           = 0x904A,
    GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX           = 0x904B
  };
#endif

IMPLEMENT_STANDARD_HANDLE (OpenGl_Context, Standard_Transient)
IMPLEMENT_STANDARD_RTTIEXT(OpenGl_Context, Standard_Transient)

//! Make record shorter to retrieve function pointer using variable with same name
#define FindProcShort(theStruct, theFunc) FindProc(#theFunc, theStruct->theFunc)

namespace
{
  static const Handle(OpenGl_Resource) NULL_GL_RESOURCE;
  static const GLdouble OpenGl_DefaultPlaneEq[] = {0.0, 0.0, 0.0, 0.0};
};

// =======================================================================
// function : OpenGl_Context
// purpose  :
// =======================================================================
OpenGl_Context::OpenGl_Context (const Handle(OpenGl_Caps)& theCaps)
: core12 (NULL),
  core13 (NULL),
  core14 (NULL),
  core15 (NULL),
  core20 (NULL),
  caps   (!theCaps.IsNull() ? theCaps : new OpenGl_Caps()),
  arbNPTW(Standard_False),
  arbVBO (NULL),
  arbTBO (NULL),
  arbIns (NULL),
  arbDbg (NULL),
  extFBO (NULL),
  extGS  (NULL),
  extBgra(Standard_False),
  extAnis(Standard_False),
  extPDS (Standard_False),
  atiMem (Standard_False),
  nvxMem (Standard_False),
  mySharedResources (new OpenGl_ResourcesMap()),
  myDelayed         (new OpenGl_DelayReleaseMap()),
  myReleaseQueue    (new OpenGl_ResourcesQueue()),
  myClippingState (),
  myGlLibHandle (NULL),
  myGlCore20 (NULL),
  myAnisoMax   (1),
  myMaxTexDim  (1024),
  myMaxClipPlanes (6),
  myGlVerMajor (0),
  myGlVerMinor (0),
  myRenderMode (GL_RENDER),
  myIsInitialized (Standard_False),
  myIsStereoBuffers (Standard_False),
  myDrawBuffer (0)
{
#if defined(MAC_OS_X_VERSION_10_3) && !defined(MACOSX_USE_GLX)
  // Vendors can not extend functionality on this system
  // and developers are limited to OpenGL support provided by Mac OS X SDK.
  // We retrieve function pointers from system library
  // to generalize extensions support on all platforms.
  // In this way we also reach binary compatibility benefit between OS releases
  // if some newest functionality is optionally used.
  // Notice that GL version / extension availability checks are required
  // because function pointers may be available but not functionality itself
  // (depends on renderer).
  myGlLibHandle = dlopen ("/System/Library/Frameworks/OpenGL.framework/Versions/Current/OpenGL", RTLD_LAZY);
#endif

  myShaderManager = new OpenGl_ShaderManager (this);
}

// =======================================================================
// function : ~OpenGl_Context
// purpose  :
// =======================================================================
OpenGl_Context::~OpenGl_Context()
{
  // release clean up queue
  ReleaseDelayed();

  // release shared resources if any
  if (((const Handle(Standard_Transient)& )mySharedResources)->GetRefCount() <= 1)
  {
    myShaderManager.Nullify();
    for (NCollection_DataMap<TCollection_AsciiString, Handle(OpenGl_Resource)>::Iterator anIter (*mySharedResources);
         anIter.More(); anIter.Next())
    {
      anIter.Value()->Release (this);
    }
  }
  else
  {
    myShaderManager->SetContext (NULL);
  }
  mySharedResources.Nullify();
  myDelayed.Nullify();

  if (arbDbg != NULL
   && caps->contextDebug)
  {
    // reset callback
    void* aPtr = NULL;
    glGetPointerv (GL_DEBUG_CALLBACK_USER_PARAM_ARB, &aPtr);
    if (aPtr == this)
    {
      arbDbg->glDebugMessageCallbackARB (NULL, NULL);
    }
  }

  delete myGlCore20;
  delete arbVBO;
  delete arbTBO;
  delete arbIns;
  delete arbDbg;
  delete extFBO;
  delete extGS;
}

// =======================================================================
// function : MaxDegreeOfAnisotropy
// purpose  :
// =======================================================================
Standard_Integer OpenGl_Context::MaxDegreeOfAnisotropy() const
{
  return myAnisoMax;
}

// =======================================================================
// function : MaxTextureSize
// purpose  :
// =======================================================================
Standard_Integer OpenGl_Context::MaxTextureSize() const
{
  return myMaxTexDim;
}

// =======================================================================
// function : MaxClipPlanes
// purpose  :
// =======================================================================
Standard_Integer OpenGl_Context::MaxClipPlanes() const
{
  return myMaxClipPlanes;
}

// =======================================================================
// function : SetDrawBufferLeft
// purpose  :
// =======================================================================
void OpenGl_Context::SetDrawBufferLeft()
{
  switch (myDrawBuffer)
  {
    case GL_BACK_RIGHT :
    case GL_BACK :
      glDrawBuffer (GL_BACK_LEFT);
      myDrawBuffer = GL_BACK_LEFT;
      break;

    case GL_FRONT_RIGHT :
    case GL_FRONT :
      glDrawBuffer (GL_FRONT_LEFT);
      myDrawBuffer = GL_FRONT_LEFT;
      break;

    case GL_FRONT_AND_BACK :
    case GL_RIGHT :
      glDrawBuffer (GL_LEFT);
      myDrawBuffer = GL_LEFT;
      break;
  }
}

// =======================================================================
// function : SetDrawBufferRight
// purpose  :
// =======================================================================
void OpenGl_Context::SetDrawBufferRight()
{
  switch (myDrawBuffer)
  {
    case GL_BACK_LEFT :
    case GL_BACK :
      glDrawBuffer (GL_BACK_RIGHT);
      myDrawBuffer = GL_BACK_RIGHT;
      break;

    case GL_FRONT_LEFT :
    case GL_FRONT :
      glDrawBuffer (GL_FRONT_RIGHT);
      myDrawBuffer = GL_FRONT_RIGHT;
      break;

    case GL_FRONT_AND_BACK :
    case GL_LEFT :
      glDrawBuffer (GL_RIGHT);
      myDrawBuffer = GL_RIGHT;
      break;
  }
}

// =======================================================================
// function : SetDrawBufferMono
// purpose  :
// =======================================================================
void OpenGl_Context::SetDrawBufferMono()
{
  switch (myDrawBuffer)
  {
    case GL_BACK_LEFT :
    case GL_BACK_RIGHT :
      glDrawBuffer (GL_BACK);
      myDrawBuffer = GL_BACK;
      break;

    case GL_FRONT_LEFT :
    case GL_FRONT_RIGHT :
      glDrawBuffer (GL_FRONT);
      myDrawBuffer = GL_FRONT;
      break;

    case GL_LEFT :
    case GL_RIGHT :
      glDrawBuffer (GL_FRONT_AND_BACK);
      myDrawBuffer = GL_FRONT_AND_BACK;
      break;
  }
}

// =======================================================================
// function : FetchState
// purpose  :
// =======================================================================
void OpenGl_Context::FetchState()
{
  // cache feedback mode state
  glGetIntegerv (GL_RENDER_MODE, &myRenderMode);

  // cache draw buffer state
  glGetIntegerv (GL_DRAW_BUFFER, &myDrawBuffer);
}

// =======================================================================
// function : Share
// purpose  :
// =======================================================================
void OpenGl_Context::Share (const Handle(OpenGl_Context)& theShareCtx)
{
  if (!theShareCtx.IsNull())
  {
    mySharedResources = theShareCtx->mySharedResources;
    myDelayed         = theShareCtx->myDelayed;
    myReleaseQueue    = theShareCtx->myReleaseQueue;
    myShaderManager   = theShareCtx->myShaderManager;
  }
}

#if !defined(__APPLE__) || defined(MACOSX_USE_GLX)

// =======================================================================
// function : IsCurrent
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::IsCurrent() const
{
#if defined(_WIN32)
  if (myWindowDC == NULL || myGContext == NULL)
  {
    return Standard_False;
  }
  return (( (HDC )myWindowDC == wglGetCurrentDC())
      && ((HGLRC )myGContext == wglGetCurrentContext()));
#else
  if (myDisplay == NULL || myWindow == 0 || myGContext == 0)
  {
    return Standard_False;
  }

  return (   ((Display* )myDisplay  == glXGetCurrentDisplay())
       &&  ((GLXContext )myGContext == glXGetCurrentContext())
       && ((GLXDrawable )myWindow   == glXGetCurrentDrawable()));
#endif
}

// =======================================================================
// function : MakeCurrent
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::MakeCurrent()
{
#if defined(_WIN32)
  if (myWindowDC == NULL || myGContext == NULL)
  {
    Standard_ProgramError_Raise_if (myIsInitialized, "OpenGl_Context::Init() should be called before!");
    return Standard_False;
  }

  // technically it should be safe to activate already bound GL context
  // however some drivers (Intel etc.) may FAIL doing this for unknown reason
  if (IsCurrent())
  {
    myShaderManager->SetContext (this);
    return Standard_True;
  }
  else if (wglMakeCurrent ((HDC )myWindowDC, (HGLRC )myGContext) != TRUE)
  {
    // notice that glGetError() couldn't be used here!
    wchar_t* aMsgBuff = NULL;
    DWORD anErrorCode = GetLastError();
    FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, anErrorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (wchar_t* )&aMsgBuff, 0, NULL);
    TCollection_ExtendedString aMsg ("wglMakeCurrent() has failed. ");
    if (aMsgBuff != NULL)
    {
      aMsg += (Standard_ExtString )aMsgBuff;
      LocalFree (aMsgBuff);
    }
    PushMessage (GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB, GL_DEBUG_TYPE_ERROR_ARB, (unsigned int )anErrorCode, GL_DEBUG_SEVERITY_HIGH_ARB, aMsg);
    myIsInitialized = Standard_False;
    return Standard_False;
  }
#else
  if (myDisplay == NULL || myWindow == 0 || myGContext == 0)
  {
    Standard_ProgramError_Raise_if (myIsInitialized, "OpenGl_Context::Init() should be called before!");
    return Standard_False;
  }

  if (!glXMakeCurrent ((Display* )myDisplay, (GLXDrawable )myWindow, (GLXContext )myGContext))
  {
    // if there is no current context it might be impossible to use glGetError() correctly
    PushMessage (GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB, GL_DEBUG_TYPE_ERROR_ARB, 0, GL_DEBUG_SEVERITY_HIGH_ARB,
                 "glXMakeCurrent() has failed!");
    myIsInitialized = Standard_False;
    return Standard_False;
  }
#endif
  myShaderManager->SetContext (this);
  return Standard_True;
}

// =======================================================================
// function : SwapBuffers
// purpose  :
// =======================================================================
void OpenGl_Context::SwapBuffers()
{
#if defined(_WIN32)
  if ((HDC )myWindowDC != NULL)
  {
    ::SwapBuffers ((HDC )myWindowDC);
    glFlush();
  }
#else
  if ((Display* )myDisplay != NULL)
  {
    glXSwapBuffers ((Display* )myDisplay, (GLXDrawable )myWindow);
  }
#endif
}

#endif // __APPLE__

// =======================================================================
// function : findProc
// purpose  :
// =======================================================================
void* OpenGl_Context::findProc (const char* theFuncName)
{
#if defined(_WIN32)
  return wglGetProcAddress (theFuncName);
#elif defined(__APPLE__) && !defined(MACOSX_USE_GLX)
  return (myGlLibHandle != NULL) ? dlsym (myGlLibHandle, theFuncName) : NULL;
#else
  return (void* )glXGetProcAddress ((const GLubyte* )theFuncName);
#endif
}

// =======================================================================
// function : CheckExtension
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::CheckExtension (const char* theExtName) const
{
  if (theExtName  == NULL)
  {
    std::cerr << "CheckExtension called with NULL string!\n";
    return Standard_False;
  }

  // available since OpenGL 3.0
  // and the ONLY way to check extensions with OpenGL 3.1+ core profile
  /**if (IsGlGreaterEqual (3, 0))
  {
    GLint anExtNb = 0;
    glGetIntegerv (GL_NUM_EXTENSIONS, &anExtNb);
    for (GLint anIter = 0; anIter < anExtNb; ++anIter)
    {
      const char* anExtension = (const char* )core30->glGetStringi (GL_EXTENSIONS, (GLuint )anIter);
      if (anExtension[anExtNameLen] == '\0' &&
          strncmp (anExtension, theExtName, anExtNameLen) == 0)
      {
        return Standard_True;
      }
    }
    return Standard_False;
  }*/

  // use old way with huge string for all extensions
  const char* anExtString = (const char* )glGetString (GL_EXTENSIONS);
  if (anExtString == NULL)
  {
    Messenger()->Send ("TKOpenGL: glGetString (GL_EXTENSIONS) has returned NULL! No GL context?", Message_Warning);
    return Standard_False;
  }
  return CheckExtension (anExtString, theExtName);
}

// =======================================================================
// function : CheckExtension
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::CheckExtension (const char* theExtString,
                                                 const char* theExtName)
{
  if (theExtString == NULL)
  {
    return Standard_False;
  }

  // Search for theExtName in the extensions string.
  // Use of strstr() is not sufficient because extension names can be prefixes of other extension names.
  char* aPtrIter = (char* )theExtString;
  const char*  aPtrEnd      = aPtrIter + strlen (theExtString);
  const size_t anExtNameLen = strlen (theExtName);
  while (aPtrIter < aPtrEnd)
  {
    const size_t n = strcspn (aPtrIter, " ");
    if ((n == anExtNameLen) && (strncmp (aPtrIter, theExtName, anExtNameLen) == 0))
    {
      return Standard_True;
    }
    aPtrIter += (n + 1);
  }
  return Standard_False;
}

#if !defined(__APPLE__) || defined(MACOSX_USE_GLX)

// =======================================================================
// function : Init
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::Init()
{
  if (myIsInitialized)
  {
    return Standard_True;
  }

#if defined(_WIN32)
  myWindowDC = (Aspect_Handle )wglGetCurrentDC();
  myGContext = (Aspect_RenderingContext )wglGetCurrentContext();
#else
  myDisplay  = (Aspect_Display )glXGetCurrentDisplay();
  myGContext = (Aspect_RenderingContext )glXGetCurrentContext();
  myWindow   = (Aspect_Drawable )glXGetCurrentDrawable();
#endif
  if (myGContext == NULL)
  {
    return Standard_False;
  }

  init();
  myIsInitialized = Standard_True;
  return Standard_True;
}

#endif // __APPLE__

// =======================================================================
// function : Init
// purpose  :
// =======================================================================
#if defined(_WIN32)
Standard_Boolean OpenGl_Context::Init (const Aspect_Handle           theWindow,
                                       const Aspect_Handle           theWindowDC,
                                       const Aspect_RenderingContext theGContext)
#elif defined(__APPLE__) && !defined(MACOSX_USE_GLX)
Standard_Boolean OpenGl_Context::Init (const void*                   theGContext)
#else
Standard_Boolean OpenGl_Context::Init (const Aspect_Drawable         theWindow,
                                       const Aspect_Display          theDisplay,
                                       const Aspect_RenderingContext theGContext)
#endif
{
  Standard_ProgramError_Raise_if (myIsInitialized, "OpenGl_Context::Init() should be called only once!");
#if defined(_WIN32)
  myWindow   = theWindow;
  myGContext = theGContext;
  myWindowDC = theWindowDC;
#elif defined(__APPLE__) && !defined(MACOSX_USE_GLX)
  myGContext = (void* )theGContext;
#else
  myWindow   = theWindow;
  myGContext = theGContext;
  myDisplay  = theDisplay;
#endif
  if (myGContext == NULL || !MakeCurrent())
  {
    return Standard_False;
  }

  init();
  myIsInitialized = Standard_True;
  return Standard_True;
}

// =======================================================================
// function : ResetErrors
// purpose  :
// =======================================================================
void OpenGl_Context::ResetErrors()
{
  while (glGetError() != GL_NO_ERROR)
  {
    //
  }
}

// =======================================================================
// function : readGlVersion
// purpose  :
// =======================================================================
void OpenGl_Context::readGlVersion()
{
  // reset values
  myGlVerMajor = 0;
  myGlVerMinor = 0;

  // available since OpenGL 3.0
  GLint aMajor = 0, aMinor = 0;
  glGetIntegerv (GL_MAJOR_VERSION, &aMajor);
  glGetIntegerv (GL_MINOR_VERSION, &aMinor);
  // glGetError() sometimes does not report an error here even if
  // GL does not know GL_MAJOR_VERSION and GL_MINOR_VERSION constants.
  // This happens on some rendereres like e.g. Cygwin MESA.
  // Thus checking additionally if GL has put anything to
  // the output variables.
  if (glGetError() == GL_NO_ERROR && aMajor != 0 && aMinor != 0)
  {
    myGlVerMajor = aMajor;
    myGlVerMinor = aMinor;
    return;
  }
  ResetErrors();

  // Read version string.
  // Notice that only first two numbers splitted by point '2.1 XXXXX' are significant.
  // Following trash (after space) is vendor-specific.
  // New drivers also returns micro version of GL like '3.3.0' which has no meaning
  // and should be considered as vendor-specific too.
  const char* aVerStr = (const char* )glGetString (GL_VERSION);
  if (aVerStr == NULL || *aVerStr == '\0')
  {
    // invalid GL context
    return;
  }

  // parse string for major number
  char aMajorStr[32];
  char aMinorStr[32];
  size_t aMajIter = 0;
  while (aVerStr[aMajIter] >= '0' && aVerStr[aMajIter] <= '9')
  {
    ++aMajIter;
  }
  if (aMajIter == 0 || aMajIter >= sizeof(aMajorStr))
  {
    return;
  }
  memcpy (aMajorStr, aVerStr, aMajIter);
  aMajorStr[aMajIter] = '\0';

  // parse string for minor number
  aVerStr += aMajIter + 1;
  size_t aMinIter = 0;
  while (aVerStr[aMinIter] >= '0' && aVerStr[aMinIter] <= '9')
  {
    ++aMinIter;
  }
  if (aMinIter == 0 || aMinIter >= sizeof(aMinorStr))
  {
    return;
  }
  memcpy (aMinorStr, aVerStr, aMinIter);
  aMinorStr[aMinIter] = '\0';

  // read numbers
  myGlVerMajor = atoi (aMajorStr);
  myGlVerMinor = atoi (aMinorStr);

  if (myGlVerMajor <= 0)
  {
    myGlVerMajor = 0;
    myGlVerMinor = 0;
  }
}

static Standard_CString THE_DBGMSG_UNKNOWN = "UNKNOWN";
static Standard_CString THE_DBGMSG_SOURCES[] =
{
  ".OpenGL",    // GL_DEBUG_SOURCE_API_ARB
  ".WinSystem", // GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB
  ".GLSL",      // GL_DEBUG_SOURCE_SHADER_COMPILER_ARB
  ".3rdParty",  // GL_DEBUG_SOURCE_THIRD_PARTY_ARB
  "",           // GL_DEBUG_SOURCE_APPLICATION_ARB
  ".Other"      // GL_DEBUG_SOURCE_OTHER_ARB
};

static Standard_CString THE_DBGMSG_TYPES[] =
{
  "Error",           // GL_DEBUG_TYPE_ERROR_ARB
  "Deprecated",      // GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB
  "Undef. behavior", // GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB
  "Portability",     // GL_DEBUG_TYPE_PORTABILITY_ARB
  "Performance",     // GL_DEBUG_TYPE_PERFORMANCE_ARB
  "Other"            // GL_DEBUG_TYPE_OTHER_ARB
};

static Standard_CString THE_DBGMSG_SEV_HIGH   = "High";   // GL_DEBUG_SEVERITY_HIGH_ARB
static Standard_CString THE_DBGMSG_SEV_MEDIUM = "Medium"; // GL_DEBUG_SEVERITY_MEDIUM_ARB
static Standard_CString THE_DBGMSG_SEV_LOW    = "Low";    // GL_DEBUG_SEVERITY_LOW_ARB

//! Callback for GL_ARB_debug_output extension
static void APIENTRY debugCallbackWrap(unsigned int theSource,
                                       unsigned int theType,
                                       unsigned int theId,
                                       unsigned int theSeverity,
                                       int          /*theLength*/,
                                       const char*  theMessage,
                                       const void*  theUserParam)
{
  OpenGl_Context* aCtx = (OpenGl_Context* )theUserParam;
  aCtx->PushMessage (theSource, theType, theId, theSeverity, theMessage);
}

// =======================================================================
// function : PushMessage
// purpose  :
// =======================================================================
void OpenGl_Context::PushMessage (const unsigned int theSource,
                                  const unsigned int theType,
                                  const unsigned int theId,
                                  const unsigned int theSeverity,
                                  const TCollection_ExtendedString& theMessage)
{
  //OpenGl_Context* aCtx = (OpenGl_Context* )theUserParam;
  Standard_CString& aSrc = (theSource >= GL_DEBUG_SOURCE_API_ARB
                         && theSource <= GL_DEBUG_SOURCE_OTHER_ARB)
                         ? THE_DBGMSG_SOURCES[theSource - GL_DEBUG_SOURCE_API_ARB]
                         : THE_DBGMSG_UNKNOWN;
  Standard_CString& aType = (theType >= GL_DEBUG_TYPE_ERROR_ARB
                          && theType <= GL_DEBUG_TYPE_OTHER_ARB)
                          ? THE_DBGMSG_TYPES[theType - GL_DEBUG_TYPE_ERROR_ARB]
                          : THE_DBGMSG_UNKNOWN;
  Standard_CString& aSev = theSeverity == GL_DEBUG_SEVERITY_HIGH_ARB
                         ? THE_DBGMSG_SEV_HIGH
                         : (theSeverity == GL_DEBUG_SEVERITY_MEDIUM_ARB
                          ? THE_DBGMSG_SEV_MEDIUM
                          : THE_DBGMSG_SEV_LOW);
  Message_Gravity aGrav = theSeverity == GL_DEBUG_SEVERITY_HIGH_ARB
                        ? Message_Alarm
                        : (theSeverity == GL_DEBUG_SEVERITY_MEDIUM_ARB
                         ? Message_Warning
                         : Message_Info);

  TCollection_ExtendedString aMsg;
  aMsg += "TKOpenGl"; aMsg += aSrc;
  aMsg += " | Type: ";        aMsg += aType;
  aMsg += " | ID: ";          aMsg += (Standard_Integer )theId;
  aMsg += " | Severity: ";    aMsg += aSev;
  aMsg += " | Message:\n  ";
  aMsg += theMessage;

  Messenger()->Send (aMsg, aGrav);
}

// =======================================================================
// function : init
// purpose  :
// =======================================================================
void OpenGl_Context::init()
{
  // read version
  readGlVersion();

  arbNPTW = CheckExtension ("GL_ARB_texture_non_power_of_two");
  extBgra = CheckExtension ("GL_EXT_bgra");
  extAnis = CheckExtension ("GL_EXT_texture_filter_anisotropic");
  extPDS  = CheckExtension ("GL_EXT_packed_depth_stencil");
  atiMem  = CheckExtension ("GL_ATI_meminfo");
  nvxMem  = CheckExtension ("GL_NVX_gpu_memory_info");

  // get number of maximum clipping planes
  glGetIntegerv (GL_MAX_CLIP_PLANES, &myMaxClipPlanes);
  glGetIntegerv (GL_MAX_TEXTURE_SIZE, &myMaxTexDim);

  GLint aStereo;
  glGetIntegerv (GL_STEREO, &aStereo);
  myIsStereoBuffers = aStereo == 1;

  if (extAnis)
  {
    glGetIntegerv (GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &myAnisoMax);
  }

  myClippingState.Init (myMaxClipPlanes);

  // initialize debug context extension
  if (CheckExtension ("GL_ARB_debug_output"))
  {
    arbDbg = new OpenGl_ArbDbg();
    memset (arbDbg, 0, sizeof(OpenGl_ArbDbg)); // nullify whole structure
    if (!FindProcShort (arbDbg, glDebugMessageControlARB)
     || !FindProcShort (arbDbg, glDebugMessageInsertARB)
     || !FindProcShort (arbDbg, glDebugMessageCallbackARB)
     || !FindProcShort (arbDbg, glGetDebugMessageLogARB))
    {
      delete arbDbg;
      arbDbg = NULL;
    }
    if (arbDbg != NULL
     && caps->contextDebug)
    {
      // setup default callback
      arbDbg->glDebugMessageCallbackARB (debugCallbackWrap, this);
    #ifdef DEB
      glEnable (GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
    #endif
    }
  }

  // initialize VBO extension (ARB)
  if (CheckExtension ("GL_ARB_vertex_buffer_object"))
  {
    arbVBO = new OpenGl_ArbVBO();
    memset (arbVBO, 0, sizeof(OpenGl_ArbVBO)); // nullify whole structure
    if (!FindProcShort (arbVBO, glGenBuffersARB)
     || !FindProcShort (arbVBO, glBindBufferARB)
     || !FindProcShort (arbVBO, glBufferDataARB)
     || !FindProcShort (arbVBO, glDeleteBuffersARB))
    {
      delete arbVBO;
      arbVBO = NULL;
    }
  }

  // initialize TBO extension (ARB)
  if (CheckExtension ("GL_ARB_texture_buffer_object"))
  {
    arbTBO = new OpenGl_ArbTBO();
    memset (arbTBO, 0, sizeof(OpenGl_ArbTBO)); // nullify whole structure
    if (!FindProcShort (arbTBO, glTexBufferARB))
    {
      delete arbTBO;
      arbTBO = NULL;
    }
  }

  // initialize hardware instancing extension (ARB)
  if (CheckExtension ("GL_ARB_draw_instanced"))
  {
    arbIns = new OpenGl_ArbIns();
    memset (arbIns, 0, sizeof(OpenGl_ArbIns)); // nullify whole structure
    if (!FindProcShort (arbIns, glDrawArraysInstancedARB)
     || !FindProcShort (arbIns, glDrawElementsInstancedARB))
    {
      delete arbIns;
      arbIns = NULL;
    }
  }

  // initialize FBO extension (EXT)
  if (CheckExtension ("GL_EXT_framebuffer_object"))
  {
    extFBO = new OpenGl_ExtFBO();
    memset (extFBO, 0, sizeof(OpenGl_ExtFBO)); // nullify whole structure
    if (!FindProcShort (extFBO, glGenFramebuffersEXT)
     || !FindProcShort (extFBO, glDeleteFramebuffersEXT)
     || !FindProcShort (extFBO, glBindFramebufferEXT)
     || !FindProcShort (extFBO, glFramebufferTexture2DEXT)
     || !FindProcShort (extFBO, glCheckFramebufferStatusEXT)
     || !FindProcShort (extFBO, glGenRenderbuffersEXT)
     || !FindProcShort (extFBO, glDeleteRenderbuffersEXT)
     || !FindProcShort (extFBO, glBindRenderbufferEXT)
     || !FindProcShort (extFBO, glRenderbufferStorageEXT)
     || !FindProcShort (extFBO, glFramebufferRenderbufferEXT)
     || !FindProcShort (extFBO, glGenerateMipmapEXT))
    {
      delete extFBO;
      extFBO = NULL;
    }
  }

  // initialize GS extension (EXT)
  if (CheckExtension ("GL_EXT_geometry_shader4"))
  {
    extGS = new OpenGl_ExtGS();
    memset (extGS, 0, sizeof(OpenGl_ExtGS)); // nullify whole structure
    if (!FindProcShort (extGS, glProgramParameteriEXT))
    {
      delete extGS;
      extGS = NULL;
    }
  }

  myGlCore20 = new OpenGl_GlCore20();
  memset (myGlCore20, 0, sizeof(OpenGl_GlCore20)); // nullify whole structure

  // Check if OpenGL 1.2 core functionality is actually present
  Standard_Boolean hasGlCore12 = IsGlGreaterEqual (1, 2)
    && FindProcShort (myGlCore20, glBlendColor)
    && FindProcShort (myGlCore20, glBlendEquation)
    && FindProcShort (myGlCore20, glDrawRangeElements)
    && FindProcShort (myGlCore20, glTexImage3D)
    && FindProcShort (myGlCore20, glTexSubImage3D)
    && FindProcShort (myGlCore20, glCopyTexSubImage3D);

  // Check if OpenGL 1.3 core functionality is actually present
  Standard_Boolean hasGlCore13 = IsGlGreaterEqual (1, 3)
    && FindProcShort (myGlCore20, glActiveTexture)
    && FindProcShort (myGlCore20, glSampleCoverage)
    && FindProcShort (myGlCore20, glCompressedTexImage3D)
    && FindProcShort (myGlCore20, glCompressedTexImage2D)
    && FindProcShort (myGlCore20, glCompressedTexImage1D)
    && FindProcShort (myGlCore20, glCompressedTexSubImage3D)
    && FindProcShort (myGlCore20, glCompressedTexSubImage2D)
    && FindProcShort (myGlCore20, glCompressedTexSubImage1D)
    && FindProcShort (myGlCore20, glGetCompressedTexImage)
     // deprecated
    && FindProcShort (myGlCore20, glClientActiveTexture)
    && FindProcShort (myGlCore20, glMultiTexCoord1d)
    && FindProcShort (myGlCore20, glMultiTexCoord1dv)
    && FindProcShort (myGlCore20, glMultiTexCoord1f)
    && FindProcShort (myGlCore20, glMultiTexCoord1fv)
    && FindProcShort (myGlCore20, glMultiTexCoord1i)
    && FindProcShort (myGlCore20, glMultiTexCoord1iv)
    && FindProcShort (myGlCore20, glMultiTexCoord1s)
    && FindProcShort (myGlCore20, glMultiTexCoord1sv)
    && FindProcShort (myGlCore20, glMultiTexCoord2d)
    && FindProcShort (myGlCore20, glMultiTexCoord2dv)
    && FindProcShort (myGlCore20, glMultiTexCoord2f)
    && FindProcShort (myGlCore20, glMultiTexCoord2fv)
    && FindProcShort (myGlCore20, glMultiTexCoord2i)
    && FindProcShort (myGlCore20, glMultiTexCoord2iv)
    && FindProcShort (myGlCore20, glMultiTexCoord2s)
    && FindProcShort (myGlCore20, glMultiTexCoord2sv)
    && FindProcShort (myGlCore20, glMultiTexCoord3d)
    && FindProcShort (myGlCore20, glMultiTexCoord3dv)
    && FindProcShort (myGlCore20, glMultiTexCoord3f)
    && FindProcShort (myGlCore20, glMultiTexCoord3fv)
    && FindProcShort (myGlCore20, glMultiTexCoord3i)
    && FindProcShort (myGlCore20, glMultiTexCoord3iv)
    && FindProcShort (myGlCore20, glMultiTexCoord3s)
    && FindProcShort (myGlCore20, glMultiTexCoord3sv)
    && FindProcShort (myGlCore20, glMultiTexCoord4d)
    && FindProcShort (myGlCore20, glMultiTexCoord4dv)
    && FindProcShort (myGlCore20, glMultiTexCoord4f)
    && FindProcShort (myGlCore20, glMultiTexCoord4fv)
    && FindProcShort (myGlCore20, glMultiTexCoord4i)
    && FindProcShort (myGlCore20, glMultiTexCoord4iv)
    && FindProcShort (myGlCore20, glMultiTexCoord4s)
    && FindProcShort (myGlCore20, glMultiTexCoord4sv)
    && FindProcShort (myGlCore20, glLoadTransposeMatrixf)
    && FindProcShort (myGlCore20, glLoadTransposeMatrixd)
    && FindProcShort (myGlCore20, glMultTransposeMatrixf)
    && FindProcShort (myGlCore20, glMultTransposeMatrixd);

  // Check if OpenGL 1.4 core functionality is actually present
  Standard_Boolean hasGlCore14 = IsGlGreaterEqual (1, 4)
    && FindProcShort (myGlCore20, glBlendFuncSeparate)
    && FindProcShort (myGlCore20, glMultiDrawArrays)
    && FindProcShort (myGlCore20, glMultiDrawElements)
    && FindProcShort (myGlCore20, glPointParameterf)
    && FindProcShort (myGlCore20, glPointParameterfv)
    && FindProcShort (myGlCore20, glPointParameteri)
    && FindProcShort (myGlCore20, glPointParameteriv);

  // Check if OpenGL 1.5 core functionality is actually present
  Standard_Boolean hasGlCore15 = IsGlGreaterEqual (1, 5)
    && FindProcShort (myGlCore20, glGenQueries)
    && FindProcShort (myGlCore20, glDeleteQueries)
    && FindProcShort (myGlCore20, glIsQuery)
    && FindProcShort (myGlCore20, glBeginQuery)
    && FindProcShort (myGlCore20, glEndQuery)
    && FindProcShort (myGlCore20, glGetQueryiv)
    && FindProcShort (myGlCore20, glGetQueryObjectiv)
    && FindProcShort (myGlCore20, glGetQueryObjectuiv)
    && FindProcShort (myGlCore20, glBindBuffer)
    && FindProcShort (myGlCore20, glDeleteBuffers)
    && FindProcShort (myGlCore20, glGenBuffers)
    && FindProcShort (myGlCore20, glIsBuffer)
    && FindProcShort (myGlCore20, glBufferData)
    && FindProcShort (myGlCore20, glBufferSubData)
    && FindProcShort (myGlCore20, glGetBufferSubData)
    && FindProcShort (myGlCore20, glMapBuffer)
    && FindProcShort (myGlCore20, glUnmapBuffer)
    && FindProcShort (myGlCore20, glGetBufferParameteriv)
    && FindProcShort (myGlCore20, glGetBufferPointerv);

  // Check if OpenGL 2.0 core functionality is actually present
  Standard_Boolean hasGlCore20 = IsGlGreaterEqual (2, 0)
    && FindProcShort (myGlCore20, glBlendEquationSeparate)
    && FindProcShort (myGlCore20, glDrawBuffers)
    && FindProcShort (myGlCore20, glStencilOpSeparate)
    && FindProcShort (myGlCore20, glStencilFuncSeparate)
    && FindProcShort (myGlCore20, glStencilMaskSeparate)
    && FindProcShort (myGlCore20, glAttachShader)
    && FindProcShort (myGlCore20, glBindAttribLocation)
    && FindProcShort (myGlCore20, glCompileShader)
    && FindProcShort (myGlCore20, glCreateProgram)
    && FindProcShort (myGlCore20, glCreateShader)
    && FindProcShort (myGlCore20, glDeleteProgram)
    && FindProcShort (myGlCore20, glDeleteShader)
    && FindProcShort (myGlCore20, glDetachShader)
    && FindProcShort (myGlCore20, glDisableVertexAttribArray)
    && FindProcShort (myGlCore20, glEnableVertexAttribArray)
    && FindProcShort (myGlCore20, glGetActiveAttrib)
    && FindProcShort (myGlCore20, glGetActiveUniform)
    && FindProcShort (myGlCore20, glGetAttachedShaders)
    && FindProcShort (myGlCore20, glGetAttribLocation)
    && FindProcShort (myGlCore20, glGetProgramiv)
    && FindProcShort (myGlCore20, glGetProgramInfoLog)
    && FindProcShort (myGlCore20, glGetShaderiv)
    && FindProcShort (myGlCore20, glGetShaderInfoLog)
    && FindProcShort (myGlCore20, glGetShaderSource)
    && FindProcShort (myGlCore20, glGetUniformLocation)
    && FindProcShort (myGlCore20, glGetUniformfv)
    && FindProcShort (myGlCore20, glGetUniformiv)
    && FindProcShort (myGlCore20, glGetVertexAttribdv)
    && FindProcShort (myGlCore20, glGetVertexAttribfv)
    && FindProcShort (myGlCore20, glGetVertexAttribiv)
    && FindProcShort (myGlCore20, glGetVertexAttribPointerv)
    && FindProcShort (myGlCore20, glIsProgram)
    && FindProcShort (myGlCore20, glIsShader)
    && FindProcShort (myGlCore20, glLinkProgram)
    && FindProcShort (myGlCore20, glShaderSource)
    && FindProcShort (myGlCore20, glUseProgram)
    && FindProcShort (myGlCore20, glUniform1f)
    && FindProcShort (myGlCore20, glUniform2f)
    && FindProcShort (myGlCore20, glUniform3f)
    && FindProcShort (myGlCore20, glUniform4f)
    && FindProcShort (myGlCore20, glUniform1i)
    && FindProcShort (myGlCore20, glUniform2i)
    && FindProcShort (myGlCore20, glUniform3i)
    && FindProcShort (myGlCore20, glUniform4i)
    && FindProcShort (myGlCore20, glUniform1fv)
    && FindProcShort (myGlCore20, glUniform2fv)
    && FindProcShort (myGlCore20, glUniform3fv)
    && FindProcShort (myGlCore20, glUniform4fv)
    && FindProcShort (myGlCore20, glUniform1iv)
    && FindProcShort (myGlCore20, glUniform2iv)
    && FindProcShort (myGlCore20, glUniform3iv)
    && FindProcShort (myGlCore20, glUniform4iv)
    && FindProcShort (myGlCore20, glUniformMatrix2fv)
    && FindProcShort (myGlCore20, glUniformMatrix3fv)
    && FindProcShort (myGlCore20, glUniformMatrix4fv)
    && FindProcShort (myGlCore20, glValidateProgram)
    && FindProcShort (myGlCore20, glVertexAttrib1d)
    && FindProcShort (myGlCore20, glVertexAttrib1dv)
    && FindProcShort (myGlCore20, glVertexAttrib1f)
    && FindProcShort (myGlCore20, glVertexAttrib1fv)
    && FindProcShort (myGlCore20, glVertexAttrib1s)
    && FindProcShort (myGlCore20, glVertexAttrib1sv)
    && FindProcShort (myGlCore20, glVertexAttrib2d)
    && FindProcShort (myGlCore20, glVertexAttrib2dv)
    && FindProcShort (myGlCore20, glVertexAttrib2f)
    && FindProcShort (myGlCore20, glVertexAttrib2fv)
    && FindProcShort (myGlCore20, glVertexAttrib2s)
    && FindProcShort (myGlCore20, glVertexAttrib2sv)
    && FindProcShort (myGlCore20, glVertexAttrib3d)
    && FindProcShort (myGlCore20, glVertexAttrib3dv)
    && FindProcShort (myGlCore20, glVertexAttrib3f)
    && FindProcShort (myGlCore20, glVertexAttrib3fv)
    && FindProcShort (myGlCore20, glVertexAttrib3s)
    && FindProcShort (myGlCore20, glVertexAttrib3sv)
    && FindProcShort (myGlCore20, glVertexAttrib4Nbv)
    && FindProcShort (myGlCore20, glVertexAttrib4Niv)
    && FindProcShort (myGlCore20, glVertexAttrib4Nsv)
    && FindProcShort (myGlCore20, glVertexAttrib4Nub)
    && FindProcShort (myGlCore20, glVertexAttrib4Nubv)
    && FindProcShort (myGlCore20, glVertexAttrib4Nuiv)
    && FindProcShort (myGlCore20, glVertexAttrib4Nusv)
    && FindProcShort (myGlCore20, glVertexAttrib4bv)
    && FindProcShort (myGlCore20, glVertexAttrib4d)
    && FindProcShort (myGlCore20, glVertexAttrib4dv)
    && FindProcShort (myGlCore20, glVertexAttrib4f)
    && FindProcShort (myGlCore20, glVertexAttrib4fv)
    && FindProcShort (myGlCore20, glVertexAttrib4iv)
    && FindProcShort (myGlCore20, glVertexAttrib4s)
    && FindProcShort (myGlCore20, glVertexAttrib4sv)
    && FindProcShort (myGlCore20, glVertexAttrib4ubv)
    && FindProcShort (myGlCore20, glVertexAttrib4uiv)
    && FindProcShort (myGlCore20, glVertexAttrib4usv)
    && FindProcShort (myGlCore20, glVertexAttribPointer);

  if (!hasGlCore12)
  {
    myGlVerMajor = 1;
    myGlVerMinor = 1;
    return;
  }
  else
  {
    core12 = myGlCore20;
  }
  if (!hasGlCore13)
  {
    myGlVerMajor = 1;
    myGlVerMinor = 2;
    return;
  }
  else
  {
    core13 = myGlCore20;
  }
  if(!hasGlCore14)
  {
    myGlVerMajor = 1;
    myGlVerMinor = 3;
    return;
  }
  else
  {
    core14 = myGlCore20;
  }
  if(!hasGlCore15)
  {
    myGlVerMajor = 1;
    myGlVerMinor = 4;
    return;
  }
  else
  {
    core15 = myGlCore20;
  }
  if(!hasGlCore20)
  {
    myGlVerMajor = 1;
    myGlVerMinor = 5;
  }
  else
  {
    core20 = myGlCore20;
  }
}

// =======================================================================
// function : MemoryInfo
// purpose  :
// =======================================================================
Standard_Size OpenGl_Context::AvailableMemory() const
{
  if (atiMem)
  {
    // this is actually information for VBO pool
    // however because pools are mostly shared
    // it can be used for total GPU memory estimations
    GLint aMemInfo[4];
    aMemInfo[0] = 0;
    glGetIntegerv (GL_VBO_FREE_MEMORY_ATI, aMemInfo);
    // returned value is in KiB, however this maybe changed in future
    return Standard_Size(aMemInfo[0]) * 1024;
  }
  else if (nvxMem)
  {
    // current available dedicated video memory (in KiB), currently unused GPU memory
    GLint aMemInfo = 0;
    glGetIntegerv (GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &aMemInfo);
    return Standard_Size(aMemInfo) * 1024;
  }
  return 0;
}

// =======================================================================
// function : MemoryInfo
// purpose  :
// =======================================================================
TCollection_AsciiString OpenGl_Context::MemoryInfo() const
{
  TCollection_AsciiString anInfo;
  if (atiMem)
  {
    GLint aValues[4];
    memset (aValues, 0, sizeof(aValues));
    glGetIntegerv (GL_VBO_FREE_MEMORY_ATI, aValues);

    // total memory free in the pool
    anInfo += TCollection_AsciiString ("  GPU free memory:    ") + (aValues[0] / 1024) + " MiB\n";

    // largest available free block in the pool
    anInfo += TCollection_AsciiString ("  Largest free block: ") + (aValues[1] / 1024) + " MiB\n";
    if (aValues[2] != aValues[0])
    {
      // total auxiliary memory free
      anInfo += TCollection_AsciiString ("  Free memory:        ") + (aValues[2] / 1024) + " MiB\n";
    }
  }
  else if (nvxMem)
  {
    //current available dedicated video memory (in KiB), currently unused GPU memory
    GLint aValue = 0;
    glGetIntegerv (GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &aValue);
    anInfo += TCollection_AsciiString ("  GPU free memory:    ") + (aValue / 1024) + " MiB\n";

    // dedicated video memory, total size (in KiB) of the GPU memory
    GLint aDedicated = 0;
    glGetIntegerv (GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &aDedicated);
    anInfo += TCollection_AsciiString ("  GPU memory:         ") + (aDedicated / 1024) + " MiB\n";

    // total available memory, total size (in KiB) of the memory available for allocations
    glGetIntegerv (GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &aValue);
    if (aValue != aDedicated)
    {
      // different only for special configurations
      anInfo += TCollection_AsciiString ("  Total memory:       ") + (aValue / 1024) + " MiB\n";
    }
  }
  return anInfo;
}


// =======================================================================
// function : GetResource
// purpose  :
// =======================================================================
const Handle(OpenGl_Resource)& OpenGl_Context::GetResource (const TCollection_AsciiString& theKey) const
{
  return mySharedResources->IsBound (theKey) ? mySharedResources->Find (theKey) : NULL_GL_RESOURCE;
}

// =======================================================================
// function : ShareResource
// purpose  :
// =======================================================================
Standard_Boolean OpenGl_Context::ShareResource (const TCollection_AsciiString& theKey,
                                                const Handle(OpenGl_Resource)& theResource)
{
  if (theKey.IsEmpty() || theResource.IsNull())
  {
    return Standard_False;
  }
  return mySharedResources->Bind (theKey, theResource);
}

// =======================================================================
// function : ReleaseResource
// purpose  :
// =======================================================================
void OpenGl_Context::ReleaseResource (const TCollection_AsciiString& theKey,
                                      const Standard_Boolean         theToDelay)
{
  if (!mySharedResources->IsBound (theKey))
  {
    return;
  }
  const Handle(OpenGl_Resource)& aRes = mySharedResources->Find (theKey);
  if (aRes->GetRefCount() > 1)
  {
    return;
  }

  if (theToDelay)
  {
    myDelayed->Bind (theKey, 1);
  }
  else
  {
    aRes->Release (this);
    mySharedResources->UnBind (theKey);
  }
}

// =======================================================================
// function : DelayedRelease
// purpose  :
// =======================================================================
void OpenGl_Context::DelayedRelease (Handle(OpenGl_Resource)& theResource)
{
  myReleaseQueue->Push (theResource);
  theResource.Nullify();
}

// =======================================================================
// function : ReleaseDelayed
// purpose  :
// =======================================================================
void OpenGl_Context::ReleaseDelayed()
{
  // release queued elements
  while (!myReleaseQueue->IsEmpty())
  {
    myReleaseQueue->Front()->Release (this);
    myReleaseQueue->Pop();
  }

  // release delayed shared resoruces
  NCollection_Vector<TCollection_AsciiString> aDeadList;
  for (NCollection_DataMap<TCollection_AsciiString, Standard_Integer>::Iterator anIter (*myDelayed);
       anIter.More(); anIter.Next())
  {
    if (++anIter.ChangeValue() <= 2)
    {
      continue; // postpone release one more frame to ensure noone use it periodically
    }

    const TCollection_AsciiString& aKey = anIter.Key();
    if (!mySharedResources->IsBound (aKey))
    {
      // mixed unshared strategy delayed/undelayed was used!
      aDeadList.Append (aKey);
      continue;
    }

    Handle(OpenGl_Resource)& aRes = mySharedResources->ChangeFind (aKey);
    if (aRes->GetRefCount() > 1)
    {
      // should be only 1 instance in mySharedResources
      // if not - resource was reused again
      aDeadList.Append (aKey);
      continue;
    }

    // release resource if no one requiested it more than 2 redraw calls
    aRes->Release (this);
    mySharedResources->UnBind (aKey);
    aDeadList.Append (aKey);
  }

  for (Standard_Integer anIter = 0; anIter < aDeadList.Length(); ++anIter)
  {
    myDelayed->UnBind (aDeadList.Value (anIter));
  }
}
