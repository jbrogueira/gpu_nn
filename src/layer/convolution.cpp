#include "../../include/layer/convolution.h"
#include <iostream>
#include "../../include/math.h"

Convolution::Convolution(FilterShape filtershape, Pad pad, Stride stride,
                         Filters filters, ImageShape imageshape,
                         Channels channels)
    : Layer("Convolution"), _filter_shape(filtershape),
      _pad(pad),
      _stride(stride),
      _filters(filters),
      _inp(imageshape),
      _out(0, 0),
      _channels(channels),
      batch_size(0) {
    calculate_output_size();
    initialize_weight();
    initialize_grad();
    initialize_cudnn_handles();
    initialize_kernel();
}

Convolution::~Convolution() {
    CHECK_CUDNN(cudnnDestroyTensorDescriptor(input_des));
    CHECK_CUDNN(cudnnDestroyTensorDescriptor(output_des));
    CHECK_CUDNN(cudnnDestroyFilterDescriptor(kernel_des));
    CHECK_CUDNN(cudnnDestroyConvolutionDescriptor(convolution_des));
    CHECK_CUDNN(cudnnDestroy(cudnn));
}

void Convolution::initialize_cudnn_handles() {
    CHECK_CUDNN(cudnnCreate(&cudnn));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&input_des));
    CHECK_CUDNN(cudnnCreateTensorDescriptor(&output_des));
    CHECK_CUDNN(cudnnCreateFilterDescriptor(&kernel_des));
    CHECK_CUDNN(cudnnCreateConvolutionDescriptor(&convolution_des));
    //CHECK_CUDNN(cudnnCreateConvolutionDescriptor(&convolution_des));
}

int Convolution::output_dimension() {
    return _filters.get() * _out.first() * _out.second();
}

int Convolution::output_dimension() const {
    return  _filters.get() * _out.first() * _out.second();
}

void Convolution::calculate_output_size() {
    int h_num = (_inp.first() - _filter_shape.first() + 2 * _pad.get());
    int w_num = (_inp.second() - _filter_shape.second() + 2 * _pad.get());
    if ((h_num % _stride.get()) or (w_num % _stride.get())) {
        std::stringstream ss;
        ss << "Output size is not an integer, in:\n"
           << __PRETTY_FUNCTION__ << "\ncalled from " << __FILE__ << " at "
           << __LINE__;
        throw std::invalid_argument(ss.str());
    }
    int height = h_num / _stride.get() + 1;
    int width = w_num / _stride.get() + 1;
    _out = ImageShape(height, width);
}

void Convolution::initialize_tensors(int new_batch_size) {
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(input_des,
                                           /*format=*/CUDNN_TENSOR_NHWC,
                                           /*dataType=*/CUDNN_DATA_FLOAT,
                                           /*batch_size=*/new_batch_size,
                                           /*channels=*/_channels.get(),
                                           /*image_height=*/_inp.get().first,
                                           /*image_width=*/_inp.get().second));
    CHECK_CUDNN(cudnnSetTensor4dDescriptor(output_des,
                                           /*format=*/CUDNN_TENSOR_NHWC,
                                           /*dataType=*/CUDNN_DATA_FLOAT,
                                           /*batch_size=*/new_batch_size,
                                           /*channels=*/_filters.get(),
                                           /*image_height=*/_out.get().first,
                                           /*image_width=*/_out.get().second));
}

void Convolution::resize_gpu(int new_batch_size) {
    initialize_tensors(new_batch_size);
    initialize_algorithm();
    allocate_memory();
}

void Convolution::allocate_memory() {
    size_t bytes;
    CHECK_CUDNN(cudnnGetConvolutionForwardWorkspaceSize(
        cudnn, input_des, kernel_des, convolution_des, output_des,
        convolution_algorithm, &bytes));
    ffw.resize(bytes);
    ffw.print();
    //size_t bytes2;
    CHECK_CUDNN(cudnnGetConvolutionBackwardFilterWorkspaceSize(
        cudnn, input_des, output_des, convolution_des, kernel_des,
        convolution_bwd_algorithm, &bytes));
    bwd.resize(bytes);
    bwd.print();
    //size_t bytes3;
    CHECK_CUDNN(cudnnGetConvolutionBackwardDataWorkspaceSize(
        cudnn, kernel_des, output_des, convolution_des, input_des,
        convolution_bwd_data_algo, &bytes));
    bwd_data.resize(bytes);
    bwd_data.print();
}

void Convolution::initialize_kernel() {
    CHECK_CUDNN(cudnnSetFilter4dDescriptor(
        kernel_des,
        /*dataType=*/CUDNN_DATA_FLOAT,
        /*format=*/CUDNN_TENSOR_NHWC,
        /*out_channels=*/_filters.get(),
        /*in_channels=*/_channels.get(),
        /*kernel_height=*/_filter_shape.get().first,
        /*kernel_width=*/_filter_shape.get().second));
    //int pad = _pad.get();
    //int stride = _stride.get();
    //const int dilation = 1;
    CHECK_CUDNN(
        cudnnSetConvolution2dDescriptor(convolution_des,
                                        /*pad_height=*/_pad.get(),
                                        /*pad_width=*/_pad.get(),
                                        /*vertical_stride=*/_stride.get(),
                                        /*horizontal_stride=*/_stride.get(),
                                        /*dilation_height=*/1,
                                        /*dilation_width=*/1,
                                        /*mode=*/CUDNN_CROSS_CORRELATION,
                                        /*computeType=*/CUDNN_DATA_FLOAT));
}

void Convolution::initialize_algorithm() {
    CHECK_CUDNN(cudnnGetConvolutionForwardAlgorithm(
        cudnn, input_des, kernel_des, convolution_des, output_des,
        CUDNN_CONVOLUTION_FWD_PREFER_FASTEST,
        /*memoryLimitInBytes=*/0, &convolution_algorithm));
    CHECK_CUDNN(cudnnGetConvolutionBackwardFilterAlgorithm(
        cudnn, input_des, output_des, convolution_des, kernel_des,
        CUDNN_CONVOLUTION_BWD_FILTER_PREFER_FASTEST,
        /*memoryLimitInBytes=*/0, &convolution_bwd_algorithm));
    CHECK_CUDNN(cudnnGetConvolutionBackwardDataAlgorithm(
        cudnn, kernel_des, output_des, convolution_des, input_des,
        CUDNN_CONVOLUTION_BWD_DATA_PREFER_FASTEST,
        /*memoryLimitInBytes=*/0, &convolution_bwd_data_algo));
}

void Convolution::initialize_grad() {
    int rows = _filters.get();
    int cols = _channels.get() * _filter_shape.get().first *
               _filter_shape.get().second;
    Matrix tmp = Matrix(rows, cols).setZero();
    gradients.push_back(std::make_shared<Storage>(tmp));
}

void Convolution::initialize_weight() {
    srand((unsigned int)time(0));
    int rows = _filters.get();
    int cols = _channels.get() * _filter_shape.get().first *
               _filter_shape.get().second;
    Matrix tmp = Matrix::Random(rows, cols);
    dtype glorot_scale = std::sqrt(6.) / std::sqrt(rows + cols);
    tmp *= glorot_scale;
    parameters.push_back(std::make_shared<Storage>(tmp));
}

void Convolution::resize(int new_batch_size) {
    if (batch_size != new_batch_size) {
        resize_gpu(new_batch_size);
        resize_cpu(new_batch_size);
    }
}
void Convolution::forward_gpu(const SharedStorage& in, SharedStorage& out,
                              const std::string&) {
    resize_gpu(in->get_cols());
    const float alpha = 1.0f, beta = 0.f;
    CHECK_CUDNN(cudnnConvolutionForward(
        cudnn, &alpha, input_des, in->gpu_pointer_const(), kernel_des,
        parameters[0]->gpu_pointer_const(), convolution_des,
        CUDNN_CONVOLUTION_FWD_ALGO_IMPLICIT_PRECOMP_GEMM,
        // convolution_algorithm,
        ffw.gpu_pointer(), ffw.size(), &beta, output_des, out->gpu_pointer()));
}

void Convolution::im2col(const SharedStorage& image) {
    dtype* colp = col->cpu_pointer();
    dtype const* imagep = image->cpu_pointer_const();
    for (int t = 0; t < image->get_cols(); ++t) {
        ::im2col(imagep, _channels.get(), _inp.get().first, _inp.get().second,
                 _filter_shape.get().first, _filter_shape.get().second,
                 _pad.get(), _pad.get(), _pad.get(), _pad.get(), _stride.get(),
                 _stride.get(), colp);
        imagep += _inp.get().first * _inp.get().second * _channels.get();
        colp += _filter_shape.get().first * _filter_shape.get().second *
                _channels.get() * _out.get().first * _out.get().second;
    }
}

void Convolution::resize_cpu(int new_batch_size) {
    int rows = _filter_shape.get().first * _filter_shape.get().second *
               _channels.get();
    int cols = _out.get().first * _out.get().second * new_batch_size;
    Matrix tmp = Matrix::Zero(rows, cols);
    col = std::make_shared<Storage>(tmp);
}

void Convolution::forward_cpu(const SharedStorage& in, SharedStorage& out,
                              const std::string&) {
    resize_cpu(in->get_cols());
    im2col(in);
    out->return_data() =
        parameters[0]->return_data_const() * col->return_data_const();
}

void Convolution::backward_gpu(const SharedStorage& values,
                               const SharedStorage& gradient_in,
                               SharedStorage& gradient_out) {
    const float alpha = 1.0f, beta = 0.f;
    CHECK_CUDNN(cudnnConvolutionBackwardFilter(
        cudnn, &alpha, input_des, values->gpu_pointer_const(), output_des,
        gradient_in->gpu_pointer_const(), convolution_des,
        convolution_bwd_algorithm, bwd.gpu_pointer(), bwd.size(), &beta,
        kernel_des, gradients[0]->gpu_pointer()));
    CHECK_CUDNN(cudnnConvolutionBackwardData(
        cudnn, &alpha, kernel_des, parameters[0]->gpu_pointer_const(),
        output_des, gradient_in->gpu_pointer_const(), convolution_des,
        convolution_bwd_data_algo, bwd_data.gpu_pointer(), bwd_data.size(),
        &beta, input_des, gradient_out->gpu_pointer()));
};

void Convolution::backward_cpu(const SharedStorage&, const SharedStorage&,
                               SharedStorage&) {
    ;
};
