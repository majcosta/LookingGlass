#include "backend.h"

#include "com_ref.h"
#include "common/debug.h"
#include "common/windebug.h"
#include "common/array.h"

#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d11on12.h>

#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_5.h>
#include <dxgi1_6.h>

#define CACHE_SIZE 10

typedef struct DDCacheInfo
{
  D3D11_TEXTURE2D_DESC format;
  ID3D11Texture2D   ** srcTex;
  ID3D12Resource    ** d12Res;
  ID3D11Fence       ** fence;
  ID3D12Fence       ** d12Fence;
  UINT64               fenceValue;
  bool                 ready;
}
DDCacheInfo;

struct DDInstance
{
  ComScope * comScope;

  HDESK desktop;

  ID3D12Device3          ** d12device;
  ID3D11Device5          ** device;
  ID3D11DeviceContext4   ** context;
  IDXGIOutputDuplication ** dup;
  bool                      release;

  DDCacheInfo cache[CACHE_SIZE];
  DDCacheInfo * current;

  bool                          lastPosValid;
  DXGI_OUTDUPL_POINTER_POSITION lastPos;

  void   * shapeBuffer;
  unsigned shapeBufferSize;
};

struct DDInstance * this = NULL;

#define comRef_toGlobal(dst, src) \
  _comRef_toGlobal(this->comScope, dst, src)

static void d12_dd_openDesktop(void);
static bool d12_dd_handleFrameUpdate(IDXGIResource * res);

static void d12_dd_handlePointerMovement(DXGI_OUTDUPL_POINTER_POSITION * pos,
  CapturePointer * pointer, bool * changed);
static void d12_dd_handlePointerShape(
  CapturePointer * pointer, size_t size, bool * changed);

static bool d12_dd_getCache(ID3D11Texture2D * srcTex, DDCacheInfo ** result);
static bool d12_dd_convertResource(ID3D11Texture2D * srcTex,
  DDCacheInfo * cache);

static bool d12_dd_create(unsigned frameBuffers)
{
  this = calloc(1, sizeof(*this));
  if (!this)
  {
    DEBUG_ERROR("out of memory");
    return false;
  }

  return true;
}

static bool d12_dd_init(
  bool                 debug,
  ID3D12Device3      * device,
  IDXGIAdapter1      * adapter,
  IDXGIOutput        * output)
{
  bool result = false;
  HRESULT hr;

  comRef_initGlobalScope(10 + CACHE_SIZE * 2, this->comScope);
  comRef_scopePush(10);

  // try to open the desktop so we can capture the secure desktop
  d12_dd_openDesktop();

  comRef_defineLocal(IDXGIAdapter, _adapter);
  hr = IDXGIAdapter1_QueryInterface(
    adapter, &IID_IDXGIAdapter, (void **)_adapter);
  if (FAILED(hr))
  {
    DEBUG_ERROR("Failed to get the IDXGIAdapter interface");
    goto exit;
  }

  static const D3D_FEATURE_LEVEL featureLevels[] =
  {
    D3D_FEATURE_LEVEL_12_1,
    D3D_FEATURE_LEVEL_12_0,
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1
  };
  D3D_FEATURE_LEVEL featureLevel;

  // create a DirectX11 context
  comRef_defineLocal(ID3D11Device       , d11device);
  comRef_defineLocal(ID3D11DeviceContext, d11context);
  hr = D3D11CreateDevice(
    *_adapter,
    D3D_DRIVER_TYPE_UNKNOWN,
    NULL,
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT |
      (debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
    featureLevels,
    ARRAY_LENGTH(featureLevels),
    D3D11_SDK_VERSION,
    d11device,
    &featureLevel,
    d11context);

  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the D3D11Device", hr);
    goto exit;
  }

  // get the updated interfaces
  comRef_defineLocal(ID3D11DeviceContext4, d11context4);
  hr = ID3D11DeviceContext_QueryInterface(
    *d11context, &IID_ID3D11DeviceContext4, (void **)d11context4);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Context4 interface", hr);
    goto exit;
  }

  comRef_defineLocal(ID3D11Device5, d11device5);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_ID3D11Device5, (void **)d11device5);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to get the ID3D11Device5 interface", hr);
    goto exit;
  }

  // try to reduce the latency
  comRef_defineLocal(IDXGIDevice1, dxgi1);
  hr = ID3D11Device_QueryInterface(
    *d11device, &IID_IDXGIDevice1, (void **)dxgi1);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("failed to query the DXGI interface from the device", hr);
    goto exit;
  }
  IDXGIDevice1_SetMaximumFrameLatency(*dxgi1, 1);

  // duplicate the output
  comRef_defineLocal(IDXGIOutput5          , output5);
  comRef_defineLocal(IDXGIOutputDuplication, dup    );
  hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput5, (void **)output5);
  if (FAILED(hr))
  {
    DEBUG_WARN("IDXGIOutput5 is not available, "
               "please update windows for improved performance!");
    DEBUG_WARN("Falling back to IDXGIOutput1");

    comRef_defineLocal(IDXGIOutput1, output1);
    hr = IDXGIOutput_QueryInterface(output, &IID_IDXGIOutput1, (void **)output1);
    if (FAILED(hr))
    {
      DEBUG_ERROR("Failed to query IDXGIOutput1 from the output");
      goto exit;
    }

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      hr = IDXGIOutput1_DuplicateOutput(*output1, *(IUnknown **)d11device, dup);
      if (SUCCEEDED(hr))
        break;
      Sleep(200);
    }
  }
  else
  {
    static const DXGI_FORMAT supportedFormats[] =
    {
      DXGI_FORMAT_B8G8R8A8_UNORM,
      DXGI_FORMAT_R8G8B8A8_UNORM,
      DXGI_FORMAT_R16G16B16A16_FLOAT
    };

    // we try this twice in case we still get an error on re-initialization
    for (int i = 0; i < 2; ++i)
    {
      hr = IDXGIOutput5_DuplicateOutput1(
        *output5,
        *(IUnknown **)d11device,
        0,
        ARRAY_LENGTH(supportedFormats),
        supportedFormats,
        dup);

      if (SUCCEEDED(hr))
        break;

      // if access is denied we just keep trying until it isn't
      if (hr == E_ACCESSDENIED)
        --i;

      Sleep(200);
    }
  }

  if (FAILED(hr))
  {
    DEBUG_WINERROR("DuplicateOutput Failed", hr);
    goto exit;
  }

  ID3D12Device3_AddRef(device);
  comRef_toGlobal(this->d12device, &device    );
  comRef_toGlobal(this->device   , d11device5 );
  comRef_toGlobal(this->context  , d11context4);
  comRef_toGlobal(this->dup      , dup        );
  result = true;

exit:
  comRef_scopePop();
  if (!result)
    comRef_freeScope(&this->comScope);

  return result;
}

static bool d12_dd_deinit(void)
{
  if (this->release)
  {
    IDXGIOutputDuplication_ReleaseFrame(*this->dup);
    this->release = false;
  }

  if (this->desktop)
  {
    CloseDesktop(this->desktop);
    this->desktop = NULL;
  }

  comRef_freeScope(&this->comScope);
  memset(this, 0, sizeof(*this));
  return true;
}

static void d12_dd_free(void)
{
  free(this->shapeBuffer);
  free(this);
  this = NULL;
}

static CaptureResult d12_dd_hResultToCaptureResult(const HRESULT status)
{
  switch(status)
  {
    case S_OK:
      return CAPTURE_RESULT_OK;

    case DXGI_ERROR_WAIT_TIMEOUT:
      return CAPTURE_RESULT_TIMEOUT;

    case WAIT_ABANDONED:
    case DXGI_ERROR_ACCESS_LOST:
      return CAPTURE_RESULT_REINIT;

    default:
      return CAPTURE_RESULT_ERROR;
  }
}

static CaptureResult d12_dd_capture(unsigned frameBufferIndex)
{
  HRESULT hr;
  CaptureResult result = CAPTURE_RESULT_ERROR;
  comRef_scopePush(10);

  DXGI_OUTDUPL_FRAME_INFO frameInfo = {0};
  comRef_defineLocal(IDXGIResource, res);

retry:
  if (this->release)
  {
    IDXGIOutputDuplication_ReleaseFrame(*this->dup);
    this->release = false;
  }

  hr = IDXGIOutputDuplication_AcquireNextFrame(
    *this->dup, 1000, &frameInfo, res);

  result = d12_dd_hResultToCaptureResult(hr);
  if (result != CAPTURE_RESULT_OK)
  {
    if (result == CAPTURE_RESULT_ERROR)
      DEBUG_WINERROR("AcquireNextFrame failed", hr);

    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
      hr = ID3D11Device5_GetDeviceRemovedReason(*this->device);
      if (FAILED(hr))
      {
        DEBUG_WINERROR("Device Removed", hr);
        result = CAPTURE_RESULT_ERROR;
      }
    }

    goto exit;
  }

  this->release = true;

  // if we have a new frame
  if (frameInfo.LastPresentTime.QuadPart != 0)
    if (!d12_dd_handleFrameUpdate(*res))
    {
      result = CAPTURE_RESULT_ERROR;
      goto exit;
    }

  bool postPointer = false;
  bool postShape   = false;
  CapturePointer pointer = {0};

  // if the pointer has moved
  if (frameInfo.LastMouseUpdateTime.QuadPart != 0)
    d12_dd_handlePointerMovement(
      &frameInfo.PointerPosition, &pointer, &postPointer);

  // if the pointer shape has changed
  if (frameInfo.PointerShapeBufferSize > 0)
    d12_dd_handlePointerShape(
      &pointer, frameInfo.PointerShapeBufferSize, &postShape);

  if (postPointer)
    d12_updatePointer(&pointer, this->shapeBuffer, this->shapeBufferSize);

  // if this was not a frame update, go back and try again
  if (frameInfo.LastPresentTime.QuadPart == 0)
    goto retry;

exit:
  comRef_scopePop();
  return result;
}

static CaptureResult d12_dd_sync(ID3D12CommandQueue * commandQueue)
{
  if (!this->current)
    return CAPTURE_RESULT_TIMEOUT;

  DDCacheInfo * cache = this->current;
  if (ID3D11Fence_GetCompletedValue(*cache->fence) < cache->fenceValue)
    ID3D12CommandQueue_Wait(commandQueue, *cache->d12Fence, cache->fenceValue);

  return CAPTURE_RESULT_OK;
}

static ID3D12Resource * d12_dd_fetch(unsigned frameBufferIndex)
{
  if (!this->current)
    return NULL;

  ID3D12Resource_AddRef(*this->current->d12Res);
  return *this->current->d12Res;
}

static void d12_dd_openDesktop(void)
{
  this->desktop = OpenInputDesktop(0, FALSE, GENERIC_READ);
  if (!this->desktop)
    DEBUG_WINERROR("Failed to open the desktop", GetLastError());
  else
  {
    if (!SetThreadDesktop(this->desktop))
    {
      DEBUG_WINERROR("Failed to set the thread desktop", GetLastError());
      CloseDesktop(this->desktop);
      this->desktop = NULL;
    }
  }

  if (!this->desktop)
  {
    DEBUG_INFO("The above error(s) will prevent LG from being able to capture "
               "the secure desktop (UAC dialogs)");
    DEBUG_INFO("This is not a failure, please do not report this as an issue.");
    DEBUG_INFO("To fix this, install and run the Looking Glass host as a "
               "service.");
    DEBUG_INFO("looking-glass-host.exe InstallService");
  }
}

static bool d12_dd_handleFrameUpdate(IDXGIResource * res)
{
  bool result = false;
  comRef_scopePush(1);

  comRef_defineLocal(ID3D11Texture2D, srcTex);
  HRESULT hr = IDXGIResource_QueryInterface(
    res, &IID_ID3D11Texture2D, (void **)srcTex);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the D3D11Texture2D interface", hr);
    goto exit;
  }

  if (!d12_dd_getCache(*srcTex, &this->current))
    goto exit;

  /**
   * Even though we have not performed any copy/draw operations we still need to
   * use a fence. Because we share this texture with DirectX12 it is able to
   * read from it before the desktop duplication API has finished updating it.*/
  ++this->current->fenceValue;
  ID3D11DeviceContext4_Signal(
    *this->context, *this->current->fence, this->current->fenceValue);

  result = true;

exit:
  comRef_scopePop();
  return result;
}

static void d12_dd_handlePointerMovement(DXGI_OUTDUPL_POINTER_POSITION * pos,
  CapturePointer * pointer, bool * changed)
{
  bool setPos = false;

  // if the last position is valid, check against it for changes
  if (this->lastPosValid)
  {
    // update the position only if the pointer is visible and it has moved
    if (pos->Visible && (
        pos->Position.x != this->lastPos.Position.x ||
        pos->Position.y != this->lastPos.Position.y))
      setPos = true;

    // if the visibillity has changed
    if (pos->Visible != this->lastPos.Visible)
      *changed = true;
  }
  else
  {
    // update the position only if the pointer is visible
    setPos   = pos->Visible;

    // this is the first update, we need to send it
    *changed = true;
  }

  pointer->visible = pos->Visible;
  if (setPos)
  {
    pointer->positionUpdate = true;
    pointer->x              = pos->Position.x;
    pointer->y              = pos->Position.y;

    *changed = true;
  }

  memcpy(&this->lastPos, pos, sizeof(*pos));
  this->lastPosValid = true;
}

static void d12_dd_handlePointerShape(
  CapturePointer * pointer, size_t size, bool * changed)
{
  HRESULT hr;
  DXGI_OUTDUPL_POINTER_SHAPE_INFO info;

retry:
  if (this->shapeBufferSize < size)
  {
    free(this->shapeBuffer);
    this->shapeBuffer = malloc(size);
    if (!this->shapeBuffer)
    {
      DEBUG_ERROR("out of memory");
      this->shapeBufferSize = 0;
      return;
    }
    this->shapeBufferSize = size;
  }

  UINT s;
  hr = IDXGIOutputDuplication_GetFramePointerShape(
    *this->dup,
    this->shapeBufferSize,
    this->shapeBuffer,
    &s,
    &info);

  if (FAILED(hr))
  {
    if (hr == DXGI_ERROR_MORE_DATA)
    {
      size = s;
      goto retry;
    }
    DEBUG_WINERROR("Failed to get the pointer shape", hr);
    return;
  }

  switch(info.Type)
  {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR:
      pointer->format = CAPTURE_FMT_COLOR;
      break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
      pointer->format = CAPTURE_FMT_MASKED;
      break;

    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
      pointer->format = CAPTURE_FMT_MONO;
      break;

    default:
      DEBUG_ERROR("Unsupporter cursor format");
      return;
  }

  pointer->shapeUpdate = true;
  pointer->width       = info.Width;
  pointer->height      = info.Height;
  pointer->pitch       = info.Pitch;
  pointer->hx          = info.HotSpot.x;
  pointer->hy          = info.HotSpot.y;

  *changed = true;
}

static bool d12_dd_getCache(ID3D11Texture2D * srcTex, DDCacheInfo ** result)
{
  *result = NULL;
  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(srcTex, &srcDesc);

  unsigned freeSlot = CACHE_SIZE;
  for(unsigned i = 0; i < CACHE_SIZE; ++i)
  {
    DDCacheInfo * cache = &this->cache[i];
    if (!cache->ready)
    {
      freeSlot = min(freeSlot, i);
      continue;
    }

    // check for a resource match
    if (*cache->srcTex != srcTex)
      continue;

    // check if the match is not valid
    if (cache->format.Width  != srcDesc.Width  ||
        cache->format.Height != srcDesc.Height ||
        cache->format.Format != srcDesc.Format)
    {
      // break out and allow this entry to be rebuilt
      cache->ready = false;
      freeSlot = i;
      break;
    }

    // found, so return it
    *result = cache;
    return true;
  }

  // cache is full
  if (freeSlot == CACHE_SIZE)
    return false;

  // convert the resource
  if (!d12_dd_convertResource(srcTex, &this->cache[freeSlot]))
    return false;

  // return the new cache entry
  *result = &this->cache[freeSlot];
  return true;
}

static bool d12_dd_convertResource(ID3D11Texture2D * srcTex, DDCacheInfo * cache)
{
  bool result = false;
  HRESULT hr;
  comRef_scopePush(10);

  D3D11_TEXTURE2D_DESC srcDesc;
  ID3D11Texture2D_GetDesc(srcTex, &srcDesc);

  // get the DXGI resource interface so we can create the shared handle
  comRef_defineLocal(IDXGIResource1, dxgiRes);
  hr = ID3D11Texture2D_QueryInterface(
    srcTex, &IID_IDXGIResource1, (void **)dxgiRes);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to obtain the shared ID3D11Resource1 interface", hr);
    goto exit;
  }

  // create the shared handle
  HANDLE sharedHandle;
  hr = IDXGIResource1_CreateSharedHandle(
    *dxgiRes, NULL, DXGI_SHARED_RESOURCE_READ, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the shared handle", hr);
    goto exit;
  }

  // open the resource as a DirectX12 resource
  comRef_defineLocal(ID3D12Resource, dst);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Resource, (void **)dst);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the D3D12Resource from the handle", hr);
    CloseHandle(sharedHandle);
    goto exit;
  }

  // close the shared handle
  CloseHandle(sharedHandle);

  // create the sync fence
  comRef_defineLocal(ID3D11Fence, fence);
  hr = ID3D11Device5_CreateFence(
    *this->device, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, (void **)fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the fence", hr);
    goto exit;
  }

  // create the fence shared handle
  hr = ID3D11Fence_CreateSharedHandle(
    *fence, NULL, GENERIC_ALL, NULL, &sharedHandle);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to create the fence shared handle", hr);
    goto exit;
  }

  // open the fence as a DirectX12 fence
  comRef_defineLocal(ID3D12Fence, d12Fence);
  hr = ID3D12Device3_OpenSharedHandle(
    *this->d12device, sharedHandle, &IID_ID3D12Fence, (void **)d12Fence);
  if (FAILED(hr))
  {
    DEBUG_WINERROR("Failed to open the D3D12Fence from the handle", hr);
    CloseHandle(sharedHandle);
    goto exit;
  }

  // close the shared handle
  CloseHandle(sharedHandle);

  // store the details
  ID3D11Texture2D_AddRef(srcTex);
  comRef_toGlobal(cache->srcTex  , &srcTex );
  comRef_toGlobal(cache->d12Res  , dst     );
  comRef_toGlobal(cache->fence   , fence   );
  comRef_toGlobal(cache->d12Fence, d12Fence);
  memcpy(&cache->format, &srcDesc, sizeof(srcDesc));
  cache->fenceValue = 0;
  cache->ready      = true;

  result = true;
exit:
  comRef_scopePop();
  return result;
}

D12Backend D12Backend_DD =
{
  .name     = "Desktop Duplication",
  .codeName = "DD",

  .create   = d12_dd_create,
  .init     = d12_dd_init,
  .deinit   = d12_dd_deinit,
  .free     = d12_dd_free,
  .capture  = d12_dd_capture,
  .sync     = d12_dd_sync,
  .fetch    = d12_dd_fetch
};
