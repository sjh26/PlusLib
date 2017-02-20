/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

// Local includes
#include "PlusConfigure.h"
#include "vtkPlusOvrvisionProVideoSource.h"
#include "vtkPlusChannel.h"
#include "vtkPlusDataSource.h"

// VTK includes
#include <vtkImageData.h>
#include <vtkObjectFactory.h>
#include <vtksys/SystemTools.hxx>

// OpenCV includes
#include <opencv2/imgproc.hpp>
#include <opencv2/core/mat.hpp>
#if defined(PLUS_USE_OPENCL)
  #include <opencv2/core/ocl.hpp>

  // OpenCL includes
  #include <CL/cl.h>
#endif

//----------------------------------------------------------------------------

vtkStandardNewMacro(vtkPlusOvrvisionProVideoSource);

//----------------------------------------------------------------------------
vtkPlusOvrvisionProVideoSource::vtkPlusOvrvisionProVideoSource()
  : RequestedFormat(OVR::OV_CAM20VR_VGA)
  , ProcessingMode(OVR::OV_CAMQT_NONE)
  , CameraSync(false)
  , Framerate(-1)
  , Exposure(7808)
  , LeftEyeDataSource(NULL)
  , RightEyeDataSource(NULL)
{
  this->RequireImageOrientationInConfiguration = true;

  // Poll-based device
  this->StartThreadForInternalUpdates = true;
}

//----------------------------------------------------------------------------
vtkPlusOvrvisionProVideoSource::~vtkPlusOvrvisionProVideoSource()
{
  if (!this->Connected)
  {
    this->Disconnect();
  }
}

//----------------------------------------------------------------------------
void vtkPlusOvrvisionProVideoSource::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "Resolution: " << Resolution[0] << ", " << Resolution[1] << std::endl;
  os << indent << "Framerate: " << Framerate << std::endl;
  os << indent << "CameraSync: " << CameraSync << std::endl;
  os << indent << "ProcessingMode: " << ProcessingModeName << std::endl;
  os << indent << "LeftEyeDataSourceName: " << LeftEyeDataSourceName << std::endl;
  os << indent << "RightEyeDataSourceName: " << RightEyeDataSourceName << std::endl;
  os << indent << "Vendor: " << Vendor << std::endl;
}

//----------------------------------------------------------------------------
void vtkPlusOvrvisionProVideoSource::SetLeftEyeDataSourceName(const std::string& leftEyeDataSourceName)
{
  this->LeftEyeDataSourceName = leftEyeDataSourceName;
}

//----------------------------------------------------------------------------
void vtkPlusOvrvisionProVideoSource::SetVendor(const std::string& vendor)
{
  this->Vendor = vendor;
}

//----------------------------------------------------------------------------
void vtkPlusOvrvisionProVideoSource::SetProcessingModeName(const std::string& processingModeName)
{
  this->ProcessingModeName = processingModeName;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::InternalConnect()
{
  LOG_TRACE("vtkPlusOvrvisionProVideoSource::InternalConnect");

#if defined(PLUS_USE_OPENCL)
  cv::ocl::setUseOpenCL(true);
#endif

  if (!OvrvisionProHandle.Open(0, RequestedFormat, Vendor.c_str()))     // We don't need to share it with OpenGL/D3D, but in the future we could access the images in GPU memory
  {
    LOG_ERROR("Unable to connect to OvrvisionPro device.");
    return PLUS_FAIL;
  }

  Resolution[0] = OvrvisionProHandle.GetCamWidth();
  Resolution[1] = OvrvisionProHandle.GetCamHeight();
  Framerate = OvrvisionProHandle.GetCamFramerate();

  this->SetAcquisitionRate(Framerate);

  this->RegionOfInterest.offsetX = 0;
  this->RegionOfInterest.offsetY = 0;
  this->RegionOfInterest.width = this->Resolution[0];
  this->RegionOfInterest.height = this->Resolution[1];

  int frameSize[3] = { Resolution[0], Resolution[1], 1 };
  LeftEyeDataSource->SetInputFrameSize(frameSize);
  LeftEyeDataSource->SetNumberOfScalarComponents(3);
  RightEyeDataSource->SetInputFrameSize(frameSize);
  RightEyeDataSource->SetNumberOfScalarComponents(3);

  OvrvisionProHandle.SetCameraSyncMode(CameraSync);
  OvrvisionProHandle.SetCameraExposure(Exposure);

#if defined(PLUS_USE_OPENCL)
  cl_platform_id id = OvrvisionProHandle.GetPlatformId();
  cv::ocl::PlatformInfo info(&id);

  cv::ocl::attachContext(info.name(), OvrvisionProHandle.GetPlatformId(), OvrvisionProHandle.GetContext(), OvrvisionProHandle.GetDeviceId());
#endif

  this->LeftImage = cv::Mat(OvrvisionProHandle.GetCamHeight(), OvrvisionProHandle.GetCamWidth(), CV_8UC4);
  this->RightImage = cv::Mat(OvrvisionProHandle.GetCamHeight(), OvrvisionProHandle.GetCamWidth(), CV_8UC4);

#if !defined(PLUS_USE_OPENCL)
  this->LeftImage.data = OvrvisionProHandle.GetCamImageBGRA(OVR::OV_CAMEYE_LEFT);
  this->RightImage.data = OvrvisionProHandle.GetCamImageBGRA(OVR::OV_CAMEYE_RIGHT);
#endif

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::InternalDisconnect()
{
  LOG_DEBUG("vtkPlusOvrvisionProVideoSource::InternalDisconnect");

  if (OvrvisionProHandle.isOpen())
  {
    OvrvisionProHandle.Close();
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::InternalUpdate()
{
  int numErrors(0);

  // Query the SDK for the latest frames
  if (this->IsCapturingRGB)
  {
#if defined(PLUS_USE_OPENCL)
    OvrvisionProHandle.Capture(this->ProcessingMode); // Capture does not copy it to CPU

    cv::ocl::convertFromImage(OvrvisionProHandle.GetLeftCLImage(), this->LeftImageCL);
    cv::ocl::convertFromImage(OvrvisionProHandle.GetRightCLImage(), this->RightImageCL);

    cv::cvtColor(this->LeftImageCL, this->LeftImageCL, cv::COLOR_BGRA2RGB);
    cv::cvtColor(this->RightImageCL, this->RightImageCL, cv::COLOR_BGRA2RGB);

    this->LeftImageCL.copyTo(this->LeftImage);
    this->RightImageCL.copyTo(this->RightImage);
#else
    OvrvisionProHandle.PreStoreCamData(this->ProcessingMode);

    cv::cvtColor(this->LeftImage, this->LeftImage, cv::COLOR_BGRA2RGB);
    cv::cvtColor(this->RightImage, this->RightImage, cv::COLOR_BGRA2RGB);
#endif
  }
  else
  {
    // Cannot use OpenCL, because it doesn't touch gray scale data (just splits the 16 bit to two 8 bit)
    OvrvisionProHandle.PreStoreCamData(OVR::OV_CAMQT_NONE);
    cv::cvtColor(this->LeftImage, this->LeftImage, cv::COLOR_BGRA2GRAY);
    cv::cvtColor(this->RightImage, this->RightImage, cv::COLOR_BGRA2GRAY);
  }

  // Add them to our local buffers
  if (this->LeftEyeDataSource->AddItem(this->LeftImage.data,
                                       this->LeftEyeDataSource->GetInputImageOrientation(),
                                       this->LeftEyeDataSource->GetInputFrameSize(),
                                       VTK_UNSIGNED_CHAR,
                                       this->LeftImage.channels(),
                                       this->LeftImage.channels() == 3 ? US_IMG_RGB_COLOR : US_IMG_BRIGHTNESS,
                                       0,
                                       this->FrameNumber) != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to add left eye image to data source.");
    numErrors++;
  }

  if (this->RightEyeDataSource->AddItem(this->RightImage.data,
                                        this->RightEyeDataSource->GetInputImageOrientation(),
                                        this->RightEyeDataSource->GetInputFrameSize(),
                                        VTK_UNSIGNED_CHAR,
                                        this->RightImage.channels(),
                                        this->RightImage.channels() == 3 ? US_IMG_RGB_COLOR : US_IMG_BRIGHTNESS,
                                        0,
                                        this->FrameNumber) != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to add right eye image to data source.");
    numErrors++;
  }

  this->FrameNumber++;

  return numErrors == 0 ? PLUS_SUCCESS : PLUS_FAIL;
}

//----------------------------------------------------------------------------
void vtkPlusOvrvisionProVideoSource::ConfigureProcessingMode()
{
  this->ProcessingMode = OVR::OV_CAMQT_NONE;
  if (PlusCommon::IsEqualInsensitive(this->ProcessingModeName, "OV_CAMQT_DMSRMP"))
  {
    this->ProcessingMode = OVR::OV_CAMQT_DMSRMP;
  }
  else if (PlusCommon::IsEqualInsensitive(this->ProcessingModeName, "OV_CAMQT_DMS"))
  {
    this->ProcessingMode = OVR::OV_CAMQT_DMS;
  }
  else
  {
    LOG_WARNING("Unrecognized processing mode detected. Defaulting to OVR::OV_CAMQT_NONE.");
  }
}

//----------------------------------------------------------------------------
std::string vtkPlusOvrvisionProVideoSource::CamPropToString(OVR::Camprop format)
{
  switch (format)
  {
    case OVR::OV_CAM5MP_FULL:
      return "OV_CAM5MP_FULL";
    case OVR::OV_CAM5MP_FHD:
      return "OV_CAM5MP_FHD";
    case OVR::OV_CAMHD_FULL:
      return "OV_CAMHD_FULL";
    case OVR::OV_CAMVR_FULL:
      return "OV_CAMVR_FULL";
    case OVR::OV_CAMVR_WIDE:
      return "OV_CAMVR_WIDE";
    case OVR::OV_CAMVR_VGA:
      return "OV_CAMVR_VGA";
    case OVR::OV_CAMVR_QVGA:
      return "OV_CAMVR_QVGA";
    case OVR::OV_CAM20HD_FULL:
      return "OV_CAM20HD_FULL";
    case OVR::OV_CAM20VR_VGA:
    default:
      return "OV_CAM20VR_VGA";
  }
}

//----------------------------------------------------------------------------
OVR::Camprop vtkPlusOvrvisionProVideoSource::StringToCamProp(const std::string& format)
{
  // Handle strings without OV_ in front
  std::string nonConstFormat = format;
  if (nonConstFormat.find("OV_") == std::string::npos)
  {
    nonConstFormat.insert(0, "OV_");
  }

  if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAM5MP_FULL"))
  {
    return OVR::OV_CAM5MP_FULL;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAM5MP_FHD"))
  {
    return OVR::OV_CAM5MP_FHD;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAMHD_FULL"))
  {
    return OVR::OV_CAMHD_FULL;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAMVR_FULL"))
  {
    return OVR::OV_CAMVR_FULL;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAMVR_WIDE"))
  {
    return OVR::OV_CAMVR_WIDE;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAMVR_VGA"))
  {
    return OVR::OV_CAMVR_VGA;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAMVR_QVGA"))
  {
    return OVR::OV_CAMVR_QVGA;
  }
  else if (PlusCommon::IsEqualInsensitive(nonConstFormat, "OV_CAM20HD_FULL"))
  {
    return OVR::OV_CAM20HD_FULL;
  }
  else
  {
    return OVR::OV_CAM20VR_VGA;
  }
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);

  auto attr = deviceConfig->GetAttribute("RequestedFormat");
  if (attr == nullptr)
  {
    LOG_INFO("No requested format defined. Falling back to OV_CAM20VR_VGA (640x480@30fps).");
    RequestedFormat = OVR::OV_CAM20VR_VGA;
  }
  else
  {
    this->RequestedFormat = vtkPlusOvrvisionProVideoSource::StringToCamProp(attr);
  }
  XML_READ_STRING_ATTRIBUTE_REQUIRED(LeftEyeDataSourceName, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_REQUIRED(RightEyeDataSourceName, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(Vendor, deviceConfig);
  XML_READ_SCALAR_ATTRIBUTE_OPTIONAL(int, Exposure, deviceConfig);

  XML_READ_BOOL_ATTRIBUTE_OPTIONAL(CameraSync, deviceConfig);
  XML_READ_STRING_ATTRIBUTE_OPTIONAL(ProcessingModeName, deviceConfig);

  this->SetAcquisitionRate(this->Framerate);

  if (!this->ProcessingModeName.empty())
  {
    this->ConfigureProcessingMode();
  }

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(deviceConfig, rootConfigElement);

  std::string requestedFormat = vtkPlusOvrvisionProVideoSource::CamPropToString(this->RequestedFormat);
  rootConfigElement->SetAttribute("RequestedFormat", requestedFormat.c_str());

  if (this->CameraSync)
  {
    deviceConfig->SetAttribute("CameraSync", "TRUE");
  }

  if (this->ProcessingMode != OVR::OV_CAMQT_NONE)
  {
    switch (this->ProcessingMode)
    {
      case OVR::OV_CAMQT_DMS:
        deviceConfig->SetAttribute("ProcessingModeName", "OV_CAMQT_DMS");
        break;
      case OVR::OV_CAMQT_DMSRMP:
        deviceConfig->SetAttribute("ProcessingModeName", "OV_CAMQT_DMSRMP");
        break;
    }
  }

  XML_WRITE_STRING_ATTRIBUTE_IF_NOT_EMPTY(Vendor, deviceConfig);

  return PLUS_SUCCESS;
}

//-----------------------------------------------------------------------------
PlusStatus vtkPlusOvrvisionProVideoSource::NotifyConfigured()
{
  // OvrvisionSDK requires two data sources, left eye and right eye
  if (this->GetDataSource(this->LeftEyeDataSourceName, this->LeftEyeDataSource) != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to locate data source for left eye labelled: " << this->LeftEyeDataSourceName);
    return PLUS_FAIL;
  }

  if (this->GetDataSource(this->RightEyeDataSourceName, this->RightEyeDataSource) != PLUS_SUCCESS)
  {
    LOG_ERROR("Unable to locate data source for right eye labelled: " << this->RightEyeDataSourceName);
    return PLUS_FAIL;
  }

  if (this->LeftEyeDataSource->GetImageType() != this->RightEyeDataSource->GetImageType())
  {
    LOG_ERROR("Mistmatch between left and right eye data source image types.");
    return PLUS_FAIL;
  }

  if (this->LeftEyeDataSource->GetImageType() == US_IMG_RGB_COLOR)
  {
    this->IsCapturingRGB = true;
  }

  if (this->RightEyeDataSource->GetImageType() != US_IMG_RGB_COLOR)
  {
    LOG_ERROR("Right eye data source must be configured for image type US_IMG_RGB_COLOR. Aborting.");
    return PLUS_FAIL;
  }

  if (this->OutputChannels.size() != 2)
  {
    LOG_ERROR("OvrvisionPro device requires exactly 2 output channels. One for each eye.");
    return PLUS_FAIL;
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
std::string vtkPlusOvrvisionProVideoSource::GetLeftEyeDataSourceName() const
{
  return this->LeftEyeDataSourceName;
}

//----------------------------------------------------------------------------
std::string vtkPlusOvrvisionProVideoSource::GetRightEyeDataSourceName() const
{
  return this->RightEyeDataSourceName;
}

//----------------------------------------------------------------------------
std::string vtkPlusOvrvisionProVideoSource::GetProcessingModeName() const
{
  return this->ProcessingModeName;
}

//----------------------------------------------------------------------------
std::string vtkPlusOvrvisionProVideoSource::GetVendor() const
{
  return this->Vendor;
}