/*!
 * Copyright (c) 2016 by Contributors
 * \file q_fully_connected-inl.h
 * \brief Quantized FC operator
 * \author HPI-DeepLearning
*/
#ifndef MXNET_OPERATOR_Q_FULLY_CONNECTED_INL_H_
#define MXNET_OPERATOR_Q_FULLY_CONNECTED_INL_H_

#include <dmlc/logging.h>
#include <dmlc/parameter.h>
#include <mxnet/operator.h>
#include <map>
#include <vector>
#include <string>
#include <utility>
#include "../../src/operator/operator_common.h"
#include "./q_helper.h"


namespace mxnet {
namespace op {

// Declare enumeration of input order to make code more intuitive.
// These enums are only visible within this header
namespace q_fullc {
enum QFullyConnectedOpInputs {kData, kWeight, kBias};
enum QFullyConnectedOpOutputs {kOut};
enum QFullyConnectedResource {kTempSpace};
}  // fullc

struct QFullyConnectedParam : public dmlc::Parameter<QFullyConnectedParam> {
  int num_hidden;
  bool no_bias;
  unsigned int act_bit;
  DMLC_DECLARE_PARAMETER(QFullyConnectedParam) {
    // TODO(bing) add support for boolean
    DMLC_DECLARE_FIELD(num_hidden).set_lower_bound(1)
    .describe("Number of hidden nodes of the output.");
    DMLC_DECLARE_FIELD(no_bias).set_default(true)
    .describe("Whether to disable bias parameter.");
    DMLC_DECLARE_FIELD(act_bit).set_default(32).set_range(1, 32)
    .describe("Number of bits to quantize weights to.");
  }
};

/**
 * \brief This is the implementation of fully connected operator.
 * \tparam xpu The device that the op will be executed on.
 */
template<typename xpu, typename DType>
class QFullyConnectedOp : public Operator {
 public:
  explicit QFullyConnectedOp(QFullyConnectedParam p) {
    this->param_ = p;
  }

  virtual void Forward(const OpContext &ctx,
                       const std::vector<TBlob> &in_data,
                       const std::vector<OpReqType> &req,
                       const std::vector<TBlob> &out_data,
                       const std::vector<TBlob> &aux_args) {
    using namespace mshadow;
    using namespace mshadow::expr;
    if (req[q_fullc::kOut] == kNullOp) return;
    CHECK_EQ(req[q_fullc::kOut], kWriteTo);
    size_t expected = param_.no_bias ? 2 : 3;
    CHECK_EQ(in_data.size(), expected);
    CHECK_EQ(out_data.size(), 1);
    // TODO(bing): check the BLAS Handle, be careful
    // maybe need blas handle from context
    // TODO(bing): judge shape to remove flatten op
    Stream<xpu> *s = ctx.get_stream<xpu>();
#if defined(__CUDACC__)
    CHECK_EQ(s->blas_handle_ownership_, Stream<xpu>::OwnHandle)
        << "Must init CuBLAS handle in stream";
#endif  // __CUDACC__
    const TShape& ishape = in_data[q_fullc::kData].shape_;
    const TShape& oshape = out_data[q_fullc::kOut].shape_;

    Tensor<xpu, 2, DType> data = in_data[q_fullc::kData].get_with_shape<xpu, 2, DType>(
        Shape2(ishape[0], ishape.ProdShape(1, ishape.ndim())), s);
    Tensor<xpu, 2, DType> wmat = in_data[q_fullc::kWeight].get<xpu, 2, DType>(s);
    Tensor<xpu, 2, DType> out = out_data[q_fullc::kOut].get_with_shape<xpu, 2, DType>(
        Shape2(oshape[0], oshape.ProdShape(1, oshape.ndim())), s);

    if(!ctx.is_train && this->param_.act_bit == 1){
    	//XNOR based
      Tensor<xpu, 2, DType> wmat_T =
              NewTensor<xpu>(Shape2(wmat.shape_[1], wmat.shape_[0]), DType(0.0), MSHADOW_ALLOC_PAD, s);
      wmat_T = wmat.T();
      QFullyConnectedForward(data, wmat_T, out, param_);
      mshadow::FreeSpace(&wmat_T);
    }else{
  		// mf quantize weights
  		Tensor<xpu, 1, DType> w1d = in_data[q_fullc::kWeight].FlatTo1D<xpu, DType>(s);
  		Tensor<xpu, 1, DType> abs = ctx.requested[q_fullc::kTempSpace].get_space_typed<xpu, 1, DType>(w1d.shape_, w1d.stream_);
  		helper::quantize(w1d, abs, this->param_.act_bit);
  		// /mf quantize weights

  		out = dot(data, wmat.T());
  		if (!param_.no_bias) {
  		  Tensor<xpu, 1, DType> bias = in_data[q_fullc::kBias].get<xpu, 1, DType>(s);
  		  out += repmat(bias, data.size(0));
  		}
    }
  }

  virtual void Backward(const OpContext &ctx,
                        const std::vector<TBlob> &out_grad,
                        const std::vector<TBlob> &in_data,
                        const std::vector<TBlob> &out_data,
                        const std::vector<OpReqType> &req,
                        const std::vector<TBlob> &in_grad,
                        const std::vector<TBlob> &aux_args) {
    using namespace mshadow;
    using namespace mshadow::expr;
    CHECK_EQ(out_grad.size(), 1);
    size_t expected = param_.no_bias ? 2 : 3;
    CHECK(in_data.size() == expected && in_grad.size() == expected);
    CHECK_EQ(req.size(), expected);
    // TODO(bing): check the BLAS Handle, be careful
    //  maybe need blas handle from context
    Stream<xpu> *s = ctx.get_stream<xpu>();
    const TShape& ishape = in_data[q_fullc::kData].shape_;
    const TShape& oshape = out_grad[q_fullc::kOut].shape_;

    Tensor<xpu, 2, DType> data = in_data[q_fullc::kData].get_with_shape<xpu, 2, DType>(
        Shape2(ishape[0], ishape.ProdShape(1, ishape.ndim())), s);
    Tensor<xpu, 2, DType> wmat = in_data[q_fullc::kWeight].get<xpu, 2, DType>(s);
    Tensor<xpu, 2, DType> grad = out_grad[q_fullc::kOut].get_with_shape<xpu, 2, DType>(
        Shape2(oshape[0], oshape.ProdShape(1, oshape.ndim())), s);

#if defined(__CUDACC__)
    CHECK_EQ(s->blas_handle_ownership_, Stream<xpu>::OwnHandle)
        << "Must init CuBLAS handle in stream";
#endif
    //  backprop
    CHECK_NE(req[q_fullc::kWeight], kWriteInplace) << "cannot write weight inplace";
    // gradient of weight
    Tensor<xpu, 2, DType> gwmat = in_grad[q_fullc::kWeight].get<xpu, 2, DType>(s);
    Assign(gwmat, req[q_fullc::kWeight], dot(grad.T(), data));
    // gradient of bias
    if (!param_.no_bias) {
      Tensor<xpu, 1, DType> gbias = in_grad[q_fullc::kBias].get<xpu, 1, DType>(s);
      Assign(gbias, req[q_fullc::kBias], sum_rows(grad));
    }
    // gradient of data
    Tensor<xpu, 2, DType> gdata = in_grad[q_fullc::kData].get_with_shape<xpu, 2, DType>(
        Shape2(ishape[0], ishape.ProdShape(1, ishape.ndim())), s);
    Assign(gdata, req[q_fullc::kData], dot(grad, wmat));
  }

 private:
  QFullyConnectedParam param_;
};  // class FullyConnectedOp

// Decalre Factory function, used for dispatch specialization
template<typename xpu>
Operator* CreateOp(QFullyConnectedParam param, int dtype,
                   std::vector<TShape> *in_shape,
                   std::vector<TShape> *out_shape,
                   Context ctx);

#if DMLC_USE_CXX11
class QFullyConnectedProp : public OperatorProperty {
 public:
  std::vector<std::string> ListArguments() const override {
    if (!param_.no_bias) {
      return {"data", "weight", "bias"};
    } else {
      return {"data", "weight"};
    }
  }

  void Init(const std::vector<std::pair<std::string, std::string> >& kwargs) override {
    param_.Init(kwargs);
  }

  std::map<std::string, std::string> GetParams() const override {
    return param_.__DICT__();
  }

  bool InferShape(std::vector<TShape> *in_shape,
                  std::vector<TShape> *out_shape,
                  std::vector<TShape> *aux_shape) const override {
    using namespace mshadow;
    if (!param_.no_bias) {
      CHECK_EQ(in_shape->size(), 3) << "Input:[data, weight, bias]";
    } else {
      CHECK_EQ(in_shape->size(), 2) << "Input:[data, weight]";
    }
    const TShape &dshape = (*in_shape)[q_fullc::kData];
    // require data to be known
    if (dshape.ndim() ==  0) return false;

    index_t num_input = dshape.ProdShape(1, dshape.ndim());
    SHAPE_ASSIGN_CHECK(*in_shape, q_fullc::kWeight, Shape2(param_.num_hidden, num_input));
    if (!param_.no_bias) {
      SHAPE_ASSIGN_CHECK(*in_shape, q_fullc::kBias, Shape1(param_.num_hidden));
    }
    out_shape->clear();
    out_shape->push_back(Shape2(dshape[0], param_.num_hidden));
    return true;
  }

  bool InferType(std::vector<int> *in_type,
                 std::vector<int> *out_type,
                 std::vector<int> *aux_type) const override {
    CHECK_GE(in_type->size(), 1);
    int dtype = (*in_type)[0];
    CHECK_NE(dtype, -1) << "First input must have specified type";
    for (index_t i = 0; i < in_type->size(); ++i) {
      if ((*in_type)[i] == -1) {
        (*in_type)[i] = dtype;
      } else {
        CHECK_EQ((*in_type)[i], dtype) << "This layer requires uniform type. "
                                       << "Expected " << dtype << " v.s. given "
                                       << (*in_type)[i] << " at " << ListArguments()[i];
      }
    }
    out_type->clear();
    out_type->push_back(dtype);
    return true;
  }

  OperatorProperty* Copy() const override {
    QFullyConnectedProp* fc_sym = new QFullyConnectedProp();
    fc_sym->param_ = this->param_;
    return fc_sym;
  }

  std::string TypeString() const override {
    return "QFullyConnected";
  }

  // decalre dependency and inplace optimization options
  std::vector<int> DeclareBackwardDependency(
    const std::vector<int> &out_grad,
    const std::vector<int> &in_data,
    const std::vector<int> &out_data) const override {
    return {out_grad[q_fullc::kOut], in_data[q_fullc::kData], in_data[q_fullc::kWeight]};
  }

  std::vector<std::pair<int, void*> > BackwardInplaceOption(
    const std::vector<int> &out_grad,
    const std::vector<int> &in_data,
    const std::vector<int> &out_data,
    const std::vector<void*> &in_grad) const override {
    return {{in_data[q_fullc::kData], in_grad[q_fullc::kData]}};
  }

  std::vector<ResourceRequest> ForwardResource(
          const std::vector<TShape> &in_shape) const override {
    return {ResourceRequest::kTempSpace};
  }

  Operator* CreateOperator(Context ctx) const override {
    LOG(FATAL) << "Not Implemented.";
    return NULL;
  }

  Operator* CreateOperatorEx(Context ctx, std::vector<TShape> *in_shape,
                             std::vector<int> *in_type) const override;

 private:
  QFullyConnectedParam param_;
};  // class FullyConnectedSymbol
#endif
}  // namespace op
}  // namespace mxnet
#endif  // MXNET_OPERATOR_Q_FULLY_CONNECTED_INL_H_
