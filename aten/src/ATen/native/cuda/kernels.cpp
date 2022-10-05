/*
The following source file implements a sparse linear operator using cusparseLt
*/

#include "c10/core/ScalarType.h"
#include "c10/util/Half.h"
#include <torch/custom_class.h>
#include <cusparseLt.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDADataType.h>
#include <ATen/cuda/CUDAUtils.h>

#define CHECK_CUDA(func)                                      \
  {                                                           \
    cudaError_t status = (func);                              \
    TORCH_CHECK(status != cudaSuccess, "CUDA API failed at line %d with error: %s (%d)\n", \
          __LINE__,                                           \
          cudaGetErrorString(status),                         \
          status)                                             \
  }

#define CHECK_CUSPARSE(func)                                      \
  {                                                               \
    cusparseStatus_t status = (func);                             \
    TORCH_CHECK((status != CUSPARSE_STATUS_SUCCESS),             \
          "CUSPARSE API failed at line %d with error: %s (%d)\n", \
          __LINE__,                                               \
          cusparseGetErrorString(status),                         \
          status);                                                \
  }

// create a container that holds relevant data for cusparselt linear
// may need to template this class based on dtype
struct CusparseLtLinear : torch::CustomClassHolder {
    at::Tensor weight;
    cusparseLtHandle_t handle;
    cusparseLtMatDescriptor_t weight_descriptor;
    cudaStream_t stream;
    int64_t num_batches;
    cusparseLtMatmulPlan_t plan;
    cusparseOperation_t op_weight;
    cusparseOperation_t op_activation;
    // TODO: make this type a user input
    c10::Half *dA, *dB, *dC, *dD, *dA_compressed;
    int* d_valid;

    CusparseLtLinear() : stream{nullptr}, num_batches{1},
                         op_weight{CUSPARSE_OPERATION_NON_TRANSPOSE},
                         op_activation{CUSPARSE_OPERATION_NON_TRANSPOSE} {};
    CusparseLtLinear(const at::Tensor& weight, const int64_t num_batches) : weight{weight},
                                          stream{nullptr}, num_batches{num_batches},
                                          op_weight{CUSPARSE_OPERATION_NON_TRANSPOSE},
                                          op_activation{CUSPARSE_OPERATION_NON_TRANSPOSE}
                                          {};
  void init(int64_t gpu_index, const at::Tensor& activation, const at::Tensor& res, const at::Tensor& bias);
  void prune();
  void compress();
  void masked_mm();
};


// https://docs.nvidia.com/cuda/cusparselt/getting_started.html
// A, B, C, D in the above link corresponds to weight, activation, offset, and output

// does all the initial preparation stuff
void CusparseLtLinear::init(int64_t gpu_index, const at::Tensor& activation, const at::Tensor& res,
                            const at::Tensor& bias) {
  int major_cc, minor_cc;
  CHECK_CUDA(
      cudaDeviceGetAttribute(&major_cc, cudaDevAttrComputeCapabilityMajor, 0))
  CHECK_CUDA(
      cudaDeviceGetAttribute(&minor_cc, cudaDevAttrComputeCapabilityMinor, 0))
  TORCH_CHECK((!(major_cc == 8 && minor_cc == 0) &&
        !(major_cc == 8 && minor_cc == 6)),
              "cusparseLt is supported only on GPU devices with compute capability == 8.0, 8.6 current: " +
                    std::to_string(major_cc) + "." + std::to_string(minor_cc));

  // m & k are for weight I think, k & n are for activation
  // check if weight is transposed?
  auto m = weight.size(0);
  auto k = weight.size(1);
  auto n = activation.size(0);
  int64_t batch_strideA = m * k;
  int64_t batch_strideB = k * n;
  int64_t batch_strideC = m * n;

  // TODO: make these user inputs
  constexpr auto order = CUSPARSE_ORDER_ROW;
  constexpr auto type = CUDA_R_16F;
  constexpr auto compute_type = CUSPARSE_COMPUTE_16F;

  bool is_rowmajor = (order == CUSPARSE_ORDER_ROW);
  bool isA_transposed = (op_weight != CUSPARSE_OPERATION_NON_TRANSPOSE);
  bool isB_transposed = (op_activation != CUSPARSE_OPERATION_NON_TRANSPOSE);
  // TODO: may need to adjust logic if transpose is passed in
  // TODO: make variable names more descriptive of weight, activation, bias, etc..
  auto     num_A_rows     = (isA_transposed) ? k : m;
  auto     num_A_cols     = (isA_transposed) ? m : k;
  auto     num_B_rows     = (isB_transposed) ? n : k;
  auto     num_B_cols     = (isB_transposed) ? k : n;
  auto     num_C_rows     = m;
  auto     num_C_cols     = n;
  // is this dtype dependent?
  unsigned alignment      = 16;
  auto     lda            = (is_rowmajor) ? num_A_cols : num_A_rows;
  auto     ldb            = (is_rowmajor) ? num_B_cols : num_B_rows;
  auto     ldc            = (is_rowmajor) ? num_C_cols : num_C_rows;
  auto     C_size         = num_batches * batch_strideC;
  // TODO: make this a function of dtype when dtype is a user input
  auto     A_size_bytes   = num_batches * batch_strideA * sizeof(c10::Half);
  auto     B_size_bytes   = num_batches * batch_strideB * sizeof(c10::Half);
  auto     C_size_bytes   = num_batches * batch_strideC * sizeof(__half);
  auto     hA = weight.data_ptr<c10::Half>();
  auto     hB = activation.data_ptr<c10::Half>();
  // TODO: we may consider removing C or improving the usability;
  // right now, we assume it's not used
  auto     hC = new __half[C_size]();
  // T *hA, *hB, *hC;
  // CHECK_CUDA(cudaMallocHost((void**)&hA, A_size));
  // CHECK_CUDA(cudaMallocHost((void**)&hB, B_size));
  // CHECK_CUDA(cudaMallocHost((void**)&hC, C_size));

  //--------------------------------------------------------------------------
  // Device memory management
  CHECK_CUDA(cudaMalloc((void**)&dA, A_size_bytes))
  CHECK_CUDA(cudaMalloc((void**)&dB, B_size_bytes))
  CHECK_CUDA(cudaMalloc((void**)&dC, C_size_bytes))
  CHECK_CUDA(cudaMalloc((void**) &d_valid, sizeof(d_valid)))
  dD = res.data_ptr<c10::Half>();

  CHECK_CUDA(cudaMemcpy(dA, hA, A_size_bytes, cudaMemcpyHostToDevice))
  CHECK_CUDA(cudaMemcpy(dB, hB, B_size_bytes, cudaMemcpyHostToDevice))
  CHECK_CUDA(cudaMemcpy(dC, hC, C_size_bytes, cudaMemcpyHostToDevice))
  //--------------------------------------------------------------------------
  cusparseLtMatDescriptor_t activation_descriptor, matC;
  cusparseLtMatmulDescriptor_t matmul;
  cusparseLtMatmulAlgSelection_t alg_sel;
  CHECK_CUSPARSE(cusparseLtInit(&handle))
  // matrix descriptor initilization
  CHECK_CUSPARSE(cusparseLtStructuredDescriptorInit(
      &handle, &weight_descriptor, num_A_rows, num_A_cols,
      lda, alignment, type, order, CUSPARSELT_SPARSITY_50_PERCENT))
  CHECK_CUSPARSE(cusparseLtDenseDescriptorInit(
      &handle, &activation_descriptor, num_B_rows, num_B_cols, ldb, alignment, type, order))
  CHECK_CUSPARSE(cusparseLtDenseDescriptorInit(
      &handle, &matC, num_C_rows, num_C_cols, ldc, alignment, type, order))
  // matmul, algorithm selection, and plan initilization
  CHECK_CUSPARSE(cusparseLtMatmulDescriptorInit(
      &handle, &matmul, op_weight, op_activation, &weight_descriptor, &activation_descriptor, &matC, &matC, compute_type))
  CHECK_CUSPARSE(cusparseLtMatmulAlgSelectionInit(
      &handle, &alg_sel, &matmul, CUSPARSELT_MATMUL_ALG_DEFAULT))

  // SET NUM BATCHES
  CHECK_CUSPARSE( cusparseLtMatDescSetAttribute(&handle, &weight_descriptor,
                                          CUSPARSELT_MAT_NUM_BATCHES,
                                          &num_batches, sizeof((int)num_batches)) )
  CHECK_CUSPARSE( cusparseLtMatDescSetAttribute(&handle, &activation_descriptor,
                                          CUSPARSELT_MAT_NUM_BATCHES,
                                          &num_batches, sizeof(num_batches)) )
  CHECK_CUSPARSE( cusparseLtMatDescSetAttribute(&handle, &matC,
                                          CUSPARSELT_MAT_NUM_BATCHES,
                                          &num_batches, sizeof(num_batches)) )
  //--------------------------------------------------------------------------
  // SET BATCH STRIDE
  // if batch_strideA = 0, the matrix multiplication performs a broadcast of
  // the matrix A
  CHECK_CUSPARSE(  cusparseLtMatDescSetAttribute(&handle, &weight_descriptor,
                                              CUSPARSELT_MAT_BATCH_STRIDE,
                                              &batch_strideA,
                                              sizeof(batch_strideA)) )
  CHECK_CUSPARSE(  cusparseLtMatDescSetAttribute(&handle, &activation_descriptor,
                                              CUSPARSELT_MAT_BATCH_STRIDE,
                                              &batch_strideB,
                                              sizeof(batch_strideB)) )
  CHECK_CUSPARSE(  cusparseLtMatDescSetAttribute(&handle, &matC,
                                              CUSPARSELT_MAT_BATCH_STRIDE,
                                              &batch_strideC,
                                              sizeof(batch_strideC)) )
  // matmul, algorithm selection, and plan initialization
  CHECK_CUSPARSE( cusparseLtMatmulDescriptorInit(
                                          &handle, &matmul, op_weight, op_activation,
                                          &weight_descriptor, &activation_descriptor, &matC, &matC,
                                          compute_type) )
  //--------------------------------------------------------------------------
  // SET BIAS POINTER
  void* dBias;
  auto  hBias = bias.data_ptr<float>();
  CHECK_CUDA( cudaMalloc((void**) &dBias, m * sizeof(float)) )
  CHECK_CUDA( cudaMemcpy(dBias, hBias, m * sizeof(float),
                          cudaMemcpyHostToDevice) )
  CHECK_CUSPARSE( cusparseLtMatmulDescSetAttribute(&handle, &matmul,
                                              CUSPARSELT_MATMUL_BIAS_POINTER,
                                              &dBias, sizeof(dBias)) )

  int alg = 0;
  CHECK_CUSPARSE(cusparseLtMatmulAlgSetAttribute(
      &handle, &alg_sel, CUSPARSELT_MATMUL_ALG_CONFIG_ID, &alg, sizeof(alg)))
  size_t workspace_size;
  CHECK_CUSPARSE(
      cusparseLtMatmulGetWorkspace(&handle, &plan, &workspace_size))

  CHECK_CUSPARSE(cusparseLtMatmulPlanInit(
      &handle, &plan, &matmul, &alg_sel, workspace_size))
}

// see https://docs.nvidia.com/cuda/cusparselt/types.html for pruning_algo choices
void CusparseLtLinear::prune() {
  // make this a user input
  constexpr cusparseLtPruneAlg_t pruning_algo = CUSPARSELT_PRUNE_SPMMA_STRIP;
  //--------------------------------------------------------------------------
  // Prune the A matrix (in-place) and check the correcteness
  CHECK_CUSPARSE(cusparseLtSpMMAPrune2(
      &handle, &weight_descriptor, 1, op_weight, dA, dA, pruning_algo, stream))

  int *is_valid;
  CHECK_CUDA(cudaMalloc((void**)&is_valid, sizeof(int)))
  CHECK_CUSPARSE(
      cusparseLtSpMMAPruneCheck2(&handle, &weight_descriptor, 1, op_weight, dA, is_valid, stream))
  int h_is_valid = 0;
  CHECK_CUDA(cudaMemcpy(&h_is_valid, is_valid, sizeof(int), cudaMemcpyDeviceToHost))
  CHECK_CUDA(cudaFree(is_valid))

  TORCH_CHECK(h_is_valid == 0, "!!!! The matrix has been pruned in a wrong way. "
              "cusparseLtMatmul will not provided correct results");
}

void CusparseLtLinear::compress() {
  //--------------------------------------------------------------------------
  // Compress the A matrix
  size_t compressed_size;
  CHECK_CUSPARSE(
      cusparseLtSpMMACompressedSize2(&handle, &weight_descriptor, &compressed_size))
  CHECK_CUDA(cudaMalloc((void**)&dA_compressed, compressed_size))

  CHECK_CUSPARSE(
    cusparseLtSpMMACompress2(&handle, &weight_descriptor, true, op_weight, dA, dA_compressed, stream))
}

// this function assumes the weight tensor already has the mask applied
void CusparseLtLinear::masked_mm() {
  //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  // TODO: should we cache any of this?
  void* d_workspace = nullptr;
  int num_streams = 0;
  cudaStream_t* streams = nullptr;
  // TODO: make alpha and beta user inputs
  float alpha = 1.0f;
  float beta = 0.0f;

  CHECK_CUSPARSE(cusparseLtMatmul(
      &handle,
      &plan,
      &alpha,
      dA_compressed,
      dB,
      &beta,
      dC,
      dD,
      d_workspace,
      streams,
      num_streams))
}

// at::Tensor cusparselt_linear(const c10::intrusive_ptr<CusparseLtLinear>& params, const at::Tensor activation, const at::Tensor C) {
    // _cusparselt_init(params, gpu_index);
    // _cusparselt_prune(params, pruning_algo);
    // _cusparselt_compress(params);
    // _cusparselt_masked_mm(params, D);
    // return D;
// }



TORCH_LIBRARY(cusparselt, m) {
  m.class_<CusparseLtLinear>("CusparseLtLinear")
    .def("init", &CusparseLtLinear::init)
    .def("prune", &CusparseLtLinear::prune)
    .def("compress", &CusparseLtLinear::compress)
    .def("masked_mm", &CusparseLtLinear::masked_mm)
    // TODO: add the other ops
  ;
}
