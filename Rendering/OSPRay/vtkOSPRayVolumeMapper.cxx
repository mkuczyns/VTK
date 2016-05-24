/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkOSPRayVolumeMapper.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkOSPRayVolumeMapper.h"

#include "vtkCamera.h"
#include "vtkObjectFactory.h"
#include "vtkOSPRayPass.h"
#include "vtkSmartPointer.h"
#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkVolume.h"

#ifdef VTK_OPENGL2
#include <vtk_glew.h>
#include "vtkOpenGLHelper.h"
#else
#include "vtkOpenGLExtensionManager.h"
#include "vtkgl.h"
#include "vtkOpenGL.h"
#endif

#include "ospray/ospray.h"
#include <cfloat>

namespace ospray {
  //code borrowed from ospray::modules::opengl to facilitate updating
  //and linking
  //todo: use ospray's copy instead of this
  inline osp::vec3f operator*(const osp::vec3f &a, const osp::vec3f &b)
  {
    return (osp::vec3f){a.x*b.x, a.y*b.y, a.z*b.z};
  }
  inline osp::vec3f operator*(const osp::vec3f &a, float b)
  {
    return (osp::vec3f){a.x*b, a.y*b, a.z*b};
  }
  inline osp::vec3f operator/(const osp::vec3f &a, float b)
  {
    return (osp::vec3f){a.x/b, a.y/b, a.z/b};
  }
  inline osp::vec3f operator*(float b, const osp::vec3f &a)
  {
    return (osp::vec3f){a.x*b, a.y*b, a.z*b};
  }
  inline osp::vec3f operator*=(osp::vec3f a, float b)
  {
    return a = (osp::vec3f){a.x*b, a.y*b, a.z*b};
  }
  inline osp::vec3f operator-(const osp::vec3f& a, const osp::vec3f& b)
  {
    return (osp::vec3f){a.x-b.x, a.y-b.y, a.z-b.z};
  }
  inline osp::vec3f operator+(const osp::vec3f& a, const osp::vec3f& b)
  {
    return (osp::vec3f){a.x+b.x, a.y+b.y, a.z+b.z};
  }
  inline osp::vec3f cross(const osp::vec3f &a, const osp::vec3f &b)
  {
    return (osp::vec3f){a.y*b.z-a.z*b.y,
        a.z*b.x-a.x*b.z,
        a.x*b.y-a.y*b.x};
  }

  inline float dot(const osp::vec3f &a, const osp::vec3f &b)
  {
    return a.x*b.x+a.y*b.y+a.z*b.z;
  }
  inline osp::vec3f normalize(const osp::vec3f &v)
  {
    return v/sqrtf(dot(v,v));
  }

  /*! \brief Compute and return OpenGL depth values from the depth component of the given
    OSPRay framebuffer, using parameters of the current OpenGL context and assuming a
    perspective projection.

    This function automatically determines the parameters of the OpenGL perspective
    projection and camera direction / up vectors. It assumes these values match those
    provided to OSPRay (fovy, aspect, camera direction / up vectors). It then maps the
    OSPRay depth buffer and transforms it to OpenGL depth values according to the OpenGL
    perspective projection.

    The OSPRay frame buffer object must have been constructed with the OSP_FB_DEPTH flag.
  */
  OSPTexture2D getOSPDepthTextureFromOpenGLPerspective(const double &fovy,
                                                       const double &aspect,
                                                       const double &zNear,
                                                       const double &zFar,
                                                       const osp::vec3f &_cameraDir,
                                                       const osp::vec3f &_cameraUp,
                                                       const float *glDepthBuffer,
                                                       const size_t &glDepthBufferWidth,
                                                       const size_t &glDepthBufferHeight)
  {
    osp::vec3f cameraDir = (osp::vec3f&)_cameraDir;
    osp::vec3f cameraUp = (osp::vec3f&)_cameraUp;
    // this should later be done in ISPC...

    float *ospDepth = new float[glDepthBufferWidth * glDepthBufferHeight];

    // transform OpenGL depth to linear depth
    for (size_t i=0; i<glDepthBufferWidth*glDepthBufferHeight; i++) {
    const double z_n = 2.0 * glDepthBuffer[i] - 1.0;
    ospDepth[i] = 2.0 * zNear * zFar / (zFar + zNear - z_n * (zFar - zNear));
    if (isnan(ospDepth[i]))
      ospDepth[i] = FLT_MAX;
    }

    // transform from orthogonal Z depth to ray distance t
    osp::vec3f dir_du = normalize(cross(cameraDir, cameraUp));
    osp::vec3f dir_dv = normalize(cross(dir_du, cameraDir));

    const float imagePlaneSizeY = 2.f * tanf(fovy/2.f * M_PI/180.f);
    const float imagePlaneSizeX = imagePlaneSizeY * aspect;

    dir_du *= imagePlaneSizeX;
    dir_dv *= imagePlaneSizeY;

    const osp::vec3f dir_00 = cameraDir - .5f * dir_du - .5f * dir_dv;

    for (size_t j=0; j<glDepthBufferHeight; j++)
      for (size_t i=0; i<glDepthBufferWidth; i++) {
      const osp::vec3f dir_ij = normalize(dir_00 + float(i)/float(glDepthBufferWidth-1) * dir_du + float(j)/float(glDepthBufferHeight-1) * dir_dv);

      const float t = ospDepth[j*glDepthBufferWidth+i] / dot(cameraDir, dir_ij);
          ospDepth[j*glDepthBufferWidth+i] = t;
      }

    // nearest texture filtering required for depth textures -- we don't want interpolation of depth values...
    osp::vec2i texSize = {glDepthBufferWidth, glDepthBufferHeight};
    OSPTexture2D depthTexture = ospNewTexture2D((osp::vec2i&)texSize, OSP_TEXTURE_R32F, ospDepth, OSP_TEXTURE_FILTER_NEAREST);

    delete[] ospDepth;

    return depthTexture;
  }
}

vtkStandardNewMacro(vtkOSPRayVolumeMapper)

// ----------------------------------------------------------------------------
vtkOSPRayVolumeMapper::vtkOSPRayVolumeMapper()
{
}

// ----------------------------------------------------------------------------
vtkOSPRayVolumeMapper::~vtkOSPRayVolumeMapper()
{
}

// ----------------------------------------------------------------------------
void vtkOSPRayVolumeMapper::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);
}

// ----------------------------------------------------------------------------
void vtkOSPRayVolumeMapper::Render(vtkRenderer *ren,
                                   vtkVolume *vol)
{
  //TODO: all of this should be created the first, time then cached until
  //changed or deleted

  //establish ospray
  vtkSmartPointer<vtkOSPRayPass> ospray=vtkSmartPointer<vtkOSPRayPass>::New();

  //get the Z buffer from VTK's already rendered surfaces
  static OSPTexture2D glDepthTex=NULL;
  vtkRenderWindow *rwin =
    vtkRenderWindow::SafeDownCast(ren->GetVTKWindow());
  int viewportX, viewportY;
  int viewportWidth, viewportHeight;
  ren->GetTiledSizeAndOrigin(&viewportWidth,&viewportHeight,
                             &viewportX,&viewportY);
  float *ZBuffer = new float[viewportWidth*viewportHeight];
  rwin->GetZbufferData(
                       viewportX,  viewportY,
                       viewportX+viewportWidth-1,
                       viewportY+viewportHeight-1,
                       ZBuffer);
  //convert it to agree with OSPRay's ray depth formulation
  vtkCamera*cam = ren->GetActiveCamera();
  double zNear, zFar;
  double fovy, aspect;
  fovy = cam->GetViewAngle();
  aspect = double(viewportWidth)/double(viewportHeight);
  cam->GetClippingRange(zNear,zFar);
  double camUp[3];
  double camDir[3];
  cam->GetViewUp(camUp);
  cam->GetFocalPoint(camDir);
  osp::vec3f  cameraUp = {camUp[0], camUp[1], camUp[2]};
  osp::vec3f  cameraDir = {camDir[0], camDir[1], camDir[2]};
  double cameraPos[3];
  cam->GetPosition(cameraPos);
  cameraDir.x -= cameraPos[0];
  cameraDir.y -= cameraPos[1];
  cameraDir.z -= cameraPos[2];
  cameraDir = ospray::normalize(cameraDir);
  glDepthTex
    = ospray::getOSPDepthTextureFromOpenGLPerspective
    (fovy, aspect, zNear, zFar,
     (osp::vec3f&)cameraDir, (osp::vec3f&)cameraUp,
     ZBuffer, viewportWidth, viewportHeight);

  //use an ospray render pass in the background to
  //hand over the camera, lights and JUST this one volume
  vtkRenderPass* oldPass = ren->GetPass();
  vtkSmartPointer<vtkRenderer> tmpRen = vtkSmartPointer<vtkRenderer>::New();
  tmpRen->SetLayer(1); //TODO: hacked in for now
  tmpRen->SetRenderWindow(ren->GetRenderWindow());
  tmpRen->SetActiveCamera(ren->GetActiveCamera());
  tmpRen->SetBackground(ren->GetBackground());
  tmpRen->AddVolume(vol);
  tmpRen->SetPass(ospray);

  //hand over the depth image to ospray so it can terminate volume
  //rays
  //TODO: streamline this handoff
  ospray->SetMaxDepthTexture(glDepthTex);

  //ask the pass to render, it will blend onto the color buffer
  tmpRen->Render();
  tmpRen->SetErase(0);

  delete[] ZBuffer;
}
