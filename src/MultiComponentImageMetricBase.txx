#ifndef __MultiComponentImageMetricBase_txx_
#define __MultiComponentImageMetricBase_txx_

#include "MultiComponentImageMetricBase.h"

template <class TMetricTraits>
MultiComponentImageMetricBase<TMetricTraits>
::MultiComponentImageMetricBase()
{
  // Create the outputs of this filter
  this->SetPrimaryOutput(this->MakeOutput("Primary"));
  this->m_ComputeGradient = false;
  this->m_ComputeMovingDomainMask = false;
  this->m_ComputeAffine = false;
}


template <class TMetricTraits>
typename itk::DataObject::Pointer
MultiComponentImageMetricBase<TMetricTraits>
::MakeOutput(const DataObjectIdentifierType &key)
{
  if(key == "Primary")
    {
    return (MetricImageType::New()).GetPointer();
    }
  else if(key == "phi_gradient")
    {
    return (GradientImageType::New()).GetPointer();
    }
  else
    {
    return NULL;
    }
}

template <class TMetricTraits>
void
MultiComponentImageMetricBase<TMetricTraits>
::ToggleOutput(bool flag, const DataObjectIdentifierType &key)
{
  if(flag && !this->HasOutput(key))
    this->SetOutput(key, this->MakeOutput(key));
  if(!flag && this->HasOutput(key))
    this->RemoveOutput(key);
}

template <class TMetricTraits>
void
MultiComponentImageMetricBase<TMetricTraits>
::UpdateOutputs()
{
  this->ToggleOutput(m_ComputeGradient && !m_ComputeAffine, "phi_gradient");
  this->ToggleOutput(m_ComputeGradient && m_ComputeAffine, "tran_gradient");

  if(m_ComputeAffine)
    m_AffineTransformGradient = TransformType::New();
  else
    m_AffineTransformGradient = NULL;
}

template <class TMetricTraits>
void
MultiComponentImageMetricBase<TMetricTraits>
::GenerateInputRequestedRegion()
{
  // Call the superclass's implementation, which proparages output RR to input RR
  Superclass::GenerateInputRequestedRegion();

  // Set the moving image RR to maximum
  this->GetMovingImage()->SetRequestedRegionToLargestPossibleRegion();
}


template <class TMetricTraits>
void
MultiComponentImageMetricBase<TMetricTraits>
::BeforeThreadedGenerateData()
{
  // Create the statistics accumulators
  const InputImageType *fixed = this->GetFixedImage();

  // Create the prototype results vector
  m_ThreadData.clear();
  for(int i = 0; i < this->GetNumberOfThreads(); i++)
    {
    ThreadData td;
    td.comp_metric = vnl_vector<double>(fixed->GetNumberOfComponentsPerPixel(), 0.0);
    m_ThreadData.push_back(td);
    }
}

template <class TMetricTraits>
void
MultiComponentImageMetricBase<TMetricTraits>
::AfterThreadedGenerateData()
{
  // Compute summary stats
  const InputImageType *fixed = this->GetFixedImage();

  // Allocate the final result vector
  m_AccumulatedData = ThreadData();
  m_AccumulatedData.comp_metric = vnl_vector<double>(fixed->GetNumberOfComponentsPerPixel(), 0.0);

  for(int i = 0; i < m_ThreadData.size(); i++)
    {
    m_AccumulatedData.metric += m_ThreadData[i].metric;
    m_AccumulatedData.mask += m_ThreadData[i].mask;
    m_AccumulatedData.gradient += m_ThreadData[i].gradient;
    m_AccumulatedData.grad_mask += m_ThreadData[i].grad_mask;
    m_AccumulatedData.comp_metric += m_ThreadData[i].comp_metric;
    }

  // Report the normalized value
  m_MetricValue = m_AccumulatedData.metric / m_AccumulatedData.mask;

  // Compute the affine gradient
  if(m_ComputeAffine)
    {
    m_AffineTransformGradient = TransformType::New();

    vnl_vector<double> grad_metric(m_AccumulatedData.gradient.size());
    for(int j = 0; j < m_AccumulatedData.gradient.size(); j++)
      {
      grad_metric[j] =
          (this->GetGradientScalingFactor() * m_AccumulatedData.gradient[j]
           -m_MetricValue * m_AccumulatedData.grad_mask[j])
          / m_AccumulatedData.mask;
      }

    // Pack into the output
    unflatten_affine_transform(grad_metric.data_block(), m_AffineTransformGradient.GetPointer());
    }
}

template <class TMetricTraits>
vnl_vector<double>
MultiComponentImageMetricBase<TMetricTraits>
::GetAllMetricValues() const
{
  vnl_vector<double> result;
  result = m_AccumulatedData.comp_metric / m_AccumulatedData.mask;
  return result;

  /*
  vnl_vector<double> result;
  result = m_MetricStats.metric_values / m_MetricStats.num_voxels;
  return result;
  */
}

#include "itkImageLinearIteratorWithIndex.h"
#include "ImageRegionConstIteratorWithIndexOverride.h"
#include "FastLinearInterpolator.h"

/**
 * \class MultiComponentMetricWorker
 *
 * This class is an iterator/interpolator that supports metric classes. It supports
 * linear traversal through the output region, like a ImageLinearIteratorWithIndex.
 *
 * It also supports interpolating the moving image at the current location.
 */
template <class TMetricTraits, class TOutputImage>
class MultiComponentMetricWorker
{
public:
  /** Type definitions from the traits class */
  typedef typename TMetricTraits::InputImageType        InputImageType;
  typedef typename TMetricTraits::MaskImageType         MaskImageType;
  typedef typename TMetricTraits::DeformationFieldType  DeformationFieldType;
  typedef typename TMetricTraits::MetricImageType       MetricImageType;
  typedef typename TMetricTraits::GradientImageType     GradientImageType;
  typedef typename TMetricTraits::TransformType         TransformType;
  typedef typename TMetricTraits::RealType              RealType;
  typedef TOutputImage                                  OutputImageType;

  typedef MultiComponentImageMetricBase<TMetricTraits> MetricType;
  typedef typename MetricType::OutputImageRegionType RegionType;

  typedef MultiComponentMetricWorker<TMetricTraits,TOutputImage> Self;

  typedef itk::ImageLinearIteratorWithIndex<TOutputImage> IterBase;
  typedef IteratorExtender<IterBase> IterType;

  /** Determine the image dimension. */
  itkStaticConstMacro(ImageDimension, unsigned int, InputImageType::ImageDimension );

  typedef FastLinearInterpolator<typename TMetricTraits::RealType, ImageDimension> InterpType;

  typedef typename InputImageType::IndexType IndexType;
  typedef itk::ContinuousIndex<double, ImageDimension>  ContIndexType;

  typedef typename GradientImageType::PixelType GradientPixelType;

  typedef typename InputImageType::InternalPixelType InputComponentType;
  typedef typename OutputImageType::InternalPixelType OutputComponentType;

  MultiComponentMetricWorker(MetricType *metric, TOutputImage * image, const RegionType &region)
    : m_WrappedIter(image, region),
      m_Interpolator(metric->GetMovingImage()),
      m_OutputImage(image)
  {
    m_Metric = metric;
    m_Affine = (m_Metric->GetDeformationField() == NULL);
    m_Gradient = m_Metric->GetComputeGradient();
    m_LineLength = region.GetSize(0);
    m_FixedStep = m_Metric->GetFixedImage()->GetNumberOfComponentsPerPixel();
    m_OutputStep = image->GetNumberOfComponentsPerPixel();

    m_MovingSample = new RealType[m_FixedStep];
    m_MovingSampleGradient = new RealType * [m_FixedStep];
    for(int i = 0; i < m_FixedStep; i++)
      m_MovingSampleGradient[i] = new RealType[ImageDimension];
    m_MaskGradient = new RealType[ImageDimension];

    m_SamplePos = vnl_vector<float>(ImageDimension, 0.0f);
    m_SampleStep = vnl_vector<float>(ImageDimension, 0.0f);

    this->SetupLine();
  }

  ~MultiComponentMetricWorker()
  {
    for(int i = 0; i < m_FixedStep; i++)
      delete m_MovingSampleGradient[i];
    delete m_MovingSampleGradient;
    delete m_MovingSample;
    delete m_MaskGradient;
  }

  bool IsAtEnd()
  {
    return m_WrappedIter.IsAtEnd();
  }

  void NextLine()
  {
    // Go to the next line
    m_WrappedIter.NextLine();

    if(!m_WrappedIter.IsAtEnd())
      this->SetupLine();
  }

  void SetupLine()
  {
    // Get the offset of this line in pixels (not components)
    m_OffsetInPixels = m_WrappedIter.GetPosition() - m_OutputImage->GetBufferPointer();

    // Set up the arrays for this line
    m_FixedLine = m_Metric->GetFixedImage()->GetBufferPointer()
                  + m_OffsetInPixels * m_FixedStep;

    // Get the phi line
    m_PhiLine = m_Affine ? NULL
                         : m_Metric->GetDeformationField()->GetBufferPointer() + m_OffsetInPixels;

    // Get the output line
    m_OutputLine = m_OutputImage->GetBufferPointer() + m_OffsetInPixels * m_OutputStep;

    // Set the current sample position
    m_Index = m_WrappedIter.GetIndex();
    if(m_Affine)
      {
      for(int d = 0; d < ImageDimension; d++)
        {
        m_SamplePos[d] = m_Metric->GetAffineTransform()->GetOffset()[d];
        m_SampleStep[d] = m_Metric->GetAffineTransform()->GetMatrix()(d, 0);
        for(int j = 0; j < ImageDimension; j++)
          m_SamplePos[d] += m_Metric->GetAffineTransform()->GetMatrix()(d,j) * m_Index[j];
        }
      }
    else
      {
      for(int d = 0; d < ImageDimension; d++)
        {
        m_SamplePos[d] = m_Index[d] + (*m_PhiLine)[d];
        }
      }
  }

  Self &operator ++()
  {
    m_Index[0]++;
    if(m_Index[0] < m_LineLength)
      {
      m_FixedLine += m_FixedStep;
      m_OutputLine += m_OutputStep;

      if(m_Affine)
        {
        for(int d = 0; d < ImageDimension; d++)
          m_SamplePos[d] += m_SampleStep[d];
        }
      else
        {
        m_PhiLine++;
        for(int d = 0; d < ImageDimension; d++)
          m_SamplePos[d] = m_Index[d] + (*m_PhiLine)[d];
        }
      }

    return *this;
  }

  bool IsAtEndOfLine()
  {
    return m_Index[0] >= m_LineLength;
  }

  int GetLineLength() { return m_LineLength; }

  int GetLinePos() { return m_Index[0]; }

  typename InterpType::InOut Interpolate()
  {
    // Clear the moving sample
    for(int i = 0; i < m_FixedStep; i++)
      m_MovingSample[i] = 0.0;

    // Interpolate the moving image
    typename InterpType::InOut status;
    if(m_Gradient)
      {
      // Read out the status
      status = m_Interpolator.InterpolateWithGradient(
                 m_SamplePos.data_block(), m_MovingSample, m_MovingSampleGradient);

      // If the status is 'border', scale the values and the gradient by the mask
      if(status == InterpType::BORDER)
        {
        // Compute the mask
        m_Mask = m_Interpolator.GetMaskAndGradient(m_MaskGradient);
        }
      }
    else
      {
      status = m_Interpolator.Interpolate(
                 m_SamplePos.data_block(), m_MovingSample);

      // If the status is 'border', scale the values and the gradient by the mask
      if(status == InterpType::BORDER)
        {
        // Compute the mask
        m_Mask = m_Interpolator.GetMask();
        }
      }

    return status;
  }

  long GetOffsetInPixels() { return m_OffsetInPixels; }

  InputComponentType *GetFixedLine() { return m_FixedLine; }

  OutputComponentType *GetOutputLine() { return m_OutputLine; }

  const RealType *GetMovingSample() { return m_MovingSample; }

  const RealType *GetMovingSampleGradient(int k) { return m_MovingSampleGradient[k]; }

  const RealType *GetMaskGradient() { return m_MaskGradient; }

  RealType GetMask() { return m_Mask; }

  const IndexType &GetIndex() { return m_Index; }

protected:

  MetricType *m_Metric;
  OutputImageType *m_OutputImage;
  IterType m_WrappedIter;


  InputComponentType *m_FixedLine;
  typename TMetricTraits::DeformationFieldType::PixelType *m_PhiLine;
  typename TOutputImage::InternalPixelType *m_OutputLine;

  int m_LineLength;
  int m_FixedStep, m_OutputStep;
  long m_OffsetInPixels;

  IndexType m_Index;
  vnl_vector<float> m_SamplePos, m_SampleStep;

  InterpType m_Interpolator;

  RealType *m_MovingSample, **m_MovingSampleGradient, *m_MaskGradient;
  RealType m_Mask;

  bool m_Affine, m_Gradient;

};


#endif // __MultiComponentImageMetricBase_txx_