/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <arrow/compute/context.h>
#include <arrow/type.h>
#include <arrow/type_fwd.h>
#include <arrow/type_traits.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include "codegen/arrow_compute/ext/array_item_index.h"
#include "codegen/arrow_compute/ext/code_generator_base.h"
#include "codegen/arrow_compute/ext/codegen_common.h"
#include "codegen/arrow_compute/ext/codegen_node_visitor.h"
#include "codegen/arrow_compute/ext/codegen_register.h"
#include "codegen/arrow_compute/ext/expression_codegen_visitor.h"
#include "codegen/arrow_compute/ext/kernels_ext.h"
#include "codegen/arrow_compute/ext/typed_action_codegen_impl.h"
#include "precompile/gandiva_projector.h"
#include "utils/macros.h"

namespace sparkcolumnarplugin {
namespace codegen {
namespace arrowcompute {
namespace extra {
using ArrayList = std::vector<std::shared_ptr<arrow::Array>>;

///////////////  SortArraysToIndices  ////////////////
class HashAggregateKernel::Impl {
 public:
  Impl(arrow::compute::FunctionContext* ctx,
       std::vector<std::shared_ptr<gandiva::Node>> input_field_list,
       std::vector<std::shared_ptr<gandiva::Node>> action_list,
       std::vector<std::shared_ptr<gandiva::Node>> result_field_node_list,
       std::vector<std::shared_ptr<gandiva::Node>> result_expr_node_list)
      : ctx_(ctx), action_list_(action_list) {
    // if there is projection inside aggregate, we need to extract them into
    // projector_list
    for (auto node : input_field_list) {
      input_field_list_.push_back(
          std::dynamic_pointer_cast<gandiva::FieldNode>(node)->field());
    }
    if (result_field_node_list.size() == result_expr_node_list.size()) {
      auto tmp_size = result_field_node_list.size();
      bool no_result_project = true;
      for (int i = 0; i < tmp_size; i++) {
        if (result_field_node_list[i]->ToString() !=
            result_expr_node_list[i]->ToString()) {
          no_result_project = false;
          break;
        }
      }
      if (no_result_project) return;
    }
    for (auto node : result_field_node_list) {
      result_field_list_.push_back(
          std::dynamic_pointer_cast<gandiva::FieldNode>(node)->field());
    }
    result_expr_list_ = result_expr_node_list;
  }

  virtual arrow::Status MakeResultIterator(
      std::shared_ptr<arrow::Schema> schema,
      std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
    return arrow::Status::OK();
  }

  std::string GetSignature() { return ""; }

  arrow::Status DoCodeGen(
      int level,
      std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
          input,
      std::shared_ptr<CodeGenContext>* codegen_ctx_out, int* var_id) {
    auto codegen_ctx = std::make_shared<CodeGenContext>();

    codegen_ctx->header_codes.push_back(
        R"(#include "codegen/arrow_compute/ext/array_item_index.h")");
    codegen_ctx->header_codes.push_back(
        R"(#include "codegen/arrow_compute/ext/actions_impl.h")");

    std::vector<std::string> prepare_list;
    // 1.0 prepare aggregate input expressions
    std::stringstream prepare_ss;
    std::stringstream define_ss;
    std::stringstream aggr_prepare_ss;
    std::stringstream finish_ss;
    std::stringstream finish_condition_ss;
    std::stringstream process_ss;
    std::stringstream action_list_define_function_ss;
    std::vector<std::pair<std::string, gandiva::DataTypePtr>> action_name_list;
    std::vector<std::vector<int>> action_prepare_index_list;

    std::vector<int> key_index_list;
    std::vector<gandiva::NodePtr> prepare_function_list;
    std::vector<gandiva::NodePtr> key_node_list;
    std::vector<gandiva::FieldPtr> key_hash_field_list;
    std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
        project_output_list;

    // 1. Get action list and action_prepare_project_list
    for (auto node : action_list_) {
      auto func_node = std::dynamic_pointer_cast<gandiva::FunctionNode>(node);
      auto func_name = func_node->descriptor()->name();
      std::shared_ptr<arrow::DataType> type;
      if (func_node->children().size() > 0) {
        type = func_node->children()[0]->return_type();
      } else {
        type = func_node->return_type();
      }
      if (func_name.compare(0, 7, "action_") == 0) {
        action_name_list.push_back(std::make_pair(func_name, type));
        std::vector<int> child_prepare_idxs;
        if (func_name.compare(0, 20, "action_countLiteral_") == 0) {
          action_prepare_index_list.push_back(child_prepare_idxs);
          continue;
        }
        for (auto child_node : func_node->children()) {
          bool found = false;
          for (int i = 0; i < prepare_function_list.size(); i++) {
            auto tmp_node = prepare_function_list[i];
            if (tmp_node->ToString() == child_node->ToString()) {
              child_prepare_idxs.push_back(i);
              if (func_name == "action_groupby") {
                key_index_list.push_back(i);
                key_node_list.push_back(child_node);
              }
              found = true;
              break;
            }
          }
          if (!found) {
            if (func_name == "action_groupby") {
              key_index_list.push_back(prepare_function_list.size());
              key_node_list.push_back(child_node);
            }
            child_prepare_idxs.push_back(prepare_function_list.size());
            prepare_function_list.push_back(child_node);
          }
        }
        action_prepare_index_list.push_back(child_prepare_idxs);
      } else {
        THROW_NOT_OK(arrow::Status::Invalid("Expected some with action_ prefix."));
      }
    }

    if (key_node_list.size() > 1 ||
        (key_node_list.size() > 0 &&
         key_node_list[0]->return_type()->id() == arrow::Type::STRING)) {
      codegen_ctx->header_codes.push_back(R"(#include "precompile/hash_map.h")");
      aggr_prepare_ss << "aggr_hash_table_" << level << " = std::make_shared<"
                      << GetTypeString(arrow::utf8(), "")
                      << "HashMap>(ctx_->memory_pool());" << std::endl;
      define_ss << "std::shared_ptr<" << GetTypeString(arrow::utf8(), "")
                << "HashMap> aggr_hash_table_" << level << ";" << std::endl;

    } else if (key_node_list.size() > 0) {
      auto type = key_node_list[0]->return_type();
      codegen_ctx->header_codes.push_back(R"(#include "precompile/sparse_hash_map.h")");
      aggr_prepare_ss << "aggr_hash_table_" << level << " = std::make_shared<"
                      << "SparseHashMap<" << GetCTypeString(type)
                      << ">>(ctx_->memory_pool());" << std::endl;
      define_ss << "std::shared_ptr<SparseHashMap<" << GetCTypeString(type)
                << ">> aggr_hash_table_" << level << ";" << std::endl;
    }
    // 2. create key_hash_project_node and prepare_gandiva_project_node_list

    // 3. create cpp codes prepare_project
    int idx = 0;
    for (auto node : prepare_function_list) {
      std::vector<std::string> input_list;
      std::shared_ptr<ExpressionCodegenVisitor> project_node_visitor;
      auto is_local = false;
      RETURN_NOT_OK(MakeExpressionCodegenVisitor(node, &input, {input_field_list_}, level,
                                                 var_id, is_local, &input_list,
                                                 &project_node_visitor, false));
      auto key_name = project_node_visitor->GetResult();
      auto validity_name = project_node_visitor->GetPreCheck();

      project_output_list.push_back(std::make_pair(
          std::make_pair(key_name, project_node_visitor->GetPrepare()), nullptr));
      for (auto header : project_node_visitor->GetHeaders()) {
        if (std::find(codegen_ctx->header_codes.begin(), codegen_ctx->header_codes.end(),
                      header) == codegen_ctx->header_codes.end()) {
          codegen_ctx->header_codes.push_back(header);
        }
      }
      idx++;
    }

    // 4. create cpp codes for key_hash_project
    if (key_index_list.size() > 0) {
      auto unsafe_row_name = "aggr_key_" + std::to_string(level);
      auto unsafe_row_name_validity = unsafe_row_name + "_validity";
      if (key_index_list.size() == 1) {
        auto i = key_index_list[0];
        prepare_ss << project_output_list[i].first.second << std::endl;
        project_output_list[i].first.second = "";
        prepare_ss << "auto " << unsafe_row_name << " = "
                   << project_output_list[i].first.first << ";" << std::endl;
        prepare_ss << "auto " << unsafe_row_name_validity << " = "
                   << project_output_list[i].first.first << "_validity;" << std::endl;
      } else {
        codegen_ctx->header_codes.push_back(
            R"(#include "third_party/row_wise_memory/unsafe_row.h")");
        std::stringstream unsafe_row_define_ss;
        unsafe_row_define_ss << "std::shared_ptr<UnsafeRow> " << unsafe_row_name
                             << "_unsafe_row = std::make_shared<UnsafeRow>("
                             << key_index_list.size() << ");" << std::endl;
        codegen_ctx->unsafe_row_prepare_codes = unsafe_row_define_ss.str();
        prepare_ss << "auto " << unsafe_row_name_validity << " = "
                   << "true;" << std::endl;
        prepare_ss << unsafe_row_name << "_unsafe_row->reset();" << std::endl;
        int idx = 0;
        for (auto i : key_index_list) {
          prepare_ss << project_output_list[i].first.second << std::endl;
          project_output_list[i].first.second = "";
          auto key_name = project_output_list[i].first.first;
          auto validity_name = key_name + "_validity";
          prepare_ss << "if (" << validity_name << ") {" << std::endl;
          prepare_ss << "appendToUnsafeRow(" << unsafe_row_name << "_unsafe_row.get(), "
                     << idx << ", " << key_name << ");" << std::endl;
          prepare_ss << "} else {" << std::endl;
          prepare_ss << "setNullAt(" << unsafe_row_name << "_unsafe_row.get(), " << idx
                     << ");" << std::endl;
          prepare_ss << "}" << std::endl;
          idx++;
        }
        prepare_ss << "auto " << unsafe_row_name << " = std::string(" << unsafe_row_name
                   << "_unsafe_row->data, " << unsafe_row_name << "_unsafe_row->cursor);"
                   << std::endl;
      }
    }

    // 5. create codes for hash aggregate GetOrInsert
    std::vector<std::string> action_name_str_list;
    std::vector<std::string> action_type_str_list;
    for (auto action_pair : action_name_list) {
      action_name_str_list.push_back("\"" + action_pair.first + "\"");
      action_type_str_list.push_back("arrow::" +
                                     GetArrowTypeDefString(action_pair.second));
    }

    define_ss << "std::vector<std::shared_ptr<ActionBase>> aggr_action_list_" << level
              << ";" << std::endl;
    define_ss << "bool do_hash_aggr_finish_" << level << " = false;" << std::endl;
    define_ss << "uint64_t do_hash_aggr_finish_" << level << "_offset = 0;" << std::endl;
    define_ss << "int do_hash_aggr_finish_" << level << "_num_groups = -1;" << std::endl;
    aggr_prepare_ss << "std::vector<std::string> action_name_list_" << level << " = {"
                    << GetParameterList(action_name_str_list, false) << "};" << std::endl;
    aggr_prepare_ss << "auto action_type_list_" << level << " = {"
                    << GetParameterList(action_type_str_list, false) << "};" << std::endl;
    aggr_prepare_ss << "PrepareActionList(action_name_list_" << level
                    << ", action_type_list_" << level << ", &aggr_action_list_" << level
                    << ");" << std::endl;
    std::stringstream action_codes_ss;
    int action_idx = 0;
    for (auto idx_v : action_prepare_index_list) {
      for (auto i : idx_v) {
        action_codes_ss << project_output_list[i].first.second << std::endl;
        project_output_list[i].first.second = "";
      }
      if (idx_v.size() > 0)
        action_codes_ss << "if (" << project_output_list[idx_v[0]].first.first
                        << "_validity) {" << std::endl;
      std::vector<std::string> parameter_list;
      for (auto i : idx_v) {
        parameter_list.push_back("(void*)&" + project_output_list[i].first.first);
      }
      action_codes_ss << "RETURN_NOT_OK(aggr_action_list_" << level << "[" << action_idx
                      << "]->Evaluate(memo_index" << GetParameterList(parameter_list)
                      << "));" << std::endl;
      if (idx_v.size() > 0) {
        action_codes_ss << "} else {" << std::endl;
        action_codes_ss << "RETURN_NOT_OK(aggr_action_list_" << level << "[" << action_idx
                        << "]->EvaluateNull(memo_index));" << std::endl;
        action_codes_ss << "}" << std::endl;
      }
      action_idx++;
    }
    process_ss << "int memo_index = 0;" << std::endl;

    if (key_index_list.size() > 0) {
      process_ss << "if (!aggr_key_" << level << "_validity) {" << std::endl;
      process_ss << "  memo_index = aggr_hash_table_" << level
                 << "->GetOrInsertNull([](int){}, [](int){});" << std::endl;
      process_ss << " } else {" << std::endl;
      process_ss << "   aggr_hash_table_" << level << "->GetOrInsert(aggr_key_" << level
                 << ",[](int){}, [](int){}, &memo_index);" << std::endl;
      process_ss << " }" << std::endl;
      process_ss << action_codes_ss.str() << std::endl;
      process_ss << "if (memo_index > do_hash_aggr_finish_" << level << "_num_groups) {"
                 << std::endl;
      process_ss << "do_hash_aggr_finish_" << level << "_num_groups = memo_index;"
                 << std::endl;
      process_ss << "}" << std::endl;
    } else {
      process_ss << action_codes_ss.str() << std::endl;
    }

    action_list_define_function_ss
        << "arrow::Status PrepareActionList(std::vector<std::string> action_name_list, "
           "std::vector<std::shared_ptr<arrow::DataType>> type_list,"
           "std::vector<std::shared_ptr<ActionBase>> *action_list) {"
        << std::endl;
    action_list_define_function_ss << R"(
    int type_id = 0;
    for (int action_id = 0; action_id < action_name_list.size(); action_id++) {
      std::shared_ptr<ActionBase> action;
      if (action_name_list[action_id].compare("action_groupby") == 0) {
        RETURN_NOT_OK(MakeUniqueAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_count") == 0) {
        RETURN_NOT_OK(MakeCountAction(ctx_, &action));
      } else if (action_name_list[action_id].compare("action_sum") == 0) {
        RETURN_NOT_OK(MakeSumAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_avg") == 0) {
        RETURN_NOT_OK(MakeAvgAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_min") == 0) {
        RETURN_NOT_OK(MakeMinAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_max") == 0) {
        RETURN_NOT_OK(MakeMaxAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_sum_count") == 0) {
        RETURN_NOT_OK(MakeSumCountAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_sum_count_merge") == 0) {
        RETURN_NOT_OK(MakeSumCountMergeAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_avgByCount") == 0) {
        RETURN_NOT_OK(MakeAvgByCountAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare(0, 20, "action_countLiteral_") ==
                 0) {
        int arg = std::stoi(action_name_list[action_id].substr(20));
        RETURN_NOT_OK(MakeCountLiteralAction(ctx_, arg, &action));
      } else if (action_name_list[action_id].compare("action_stddev_samp_partial") ==
                 0) {
        RETURN_NOT_OK(MakeStddevSampPartialAction(ctx_, type_list[type_id], &action));
      } else if (action_name_list[action_id].compare("action_stddev_samp_final") == 0) {
        RETURN_NOT_OK(MakeStddevSampFinalAction(ctx_, type_list[type_id], &action));
      } else {
        return arrow::Status::NotImplemented(action_name_list[action_id],
                                             " is not implementetd.");
      }
      type_id += 1;
      (*action_list).push_back(action);
    }
    return arrow::Status::OK();
    )" << std::endl;
    action_list_define_function_ss << "}" << std::endl;

    // 6. create all input evaluated finish codes
    finish_ss << "do_hash_aggr_finish_" << level << " = true;" << std::endl;
    finish_ss << "should_stop_ = false;" << std::endl;
    finish_ss << "std::vector<std::shared_ptr<arrow::Array>> do_hash_aggr_finish_"
              << level << "_out;" << std::endl;
    finish_ss << "if(do_hash_aggr_finish_" << level << ") {";
    for (int i = 0; i < action_idx; i++) {
      finish_ss << "aggr_action_list_" << level << "[" << i
                << "]->Finish(do_hash_aggr_finish_" << level
                << "_offset, 10000, &do_hash_aggr_finish_" << level << "_out);"
                << std::endl;
    }
    finish_ss << "if (do_hash_aggr_finish_" << level << "_out.size() > 0) {" << std::endl;
    finish_ss << "auto tmp_arr = std::make_shared<Array>(do_hash_aggr_finish_" << level
              << "_out[0]);" << std::endl;
    finish_ss << "out_length += tmp_arr->length();" << std::endl;
    finish_ss << "do_hash_aggr_finish_" << level << "_offset += tmp_arr->length();"
              << std::endl;
    finish_ss << "}" << std::endl;
    finish_ss << "if (out_length == 0 || do_hash_aggr_finish_" << level
              << "_num_groups < do_hash_aggr_finish_" << level << "_offset) {"
              << std::endl;
    finish_ss << "should_stop_ = true;" << std::endl;
    finish_ss << "}" << std::endl;
    finish_ss << "}" << std::endl;

    // 7. Do GandivaProjector if result_expr_list is not empty
    if (!result_expr_list_.empty()) {
      codegen_ctx->gandiva_projector = std::make_shared<GandivaProjector>(
          ctx_, arrow::schema(result_field_list_), GetGandivaKernel(result_expr_list_));
      codegen_ctx->header_codes.push_back(R"(#include "precompile/gandiva_projector.h")");
      finish_ss << "RETURN_NOT_OK(gandiva_projector_list_[gp_idx++]->Evaluate(&do_hash_"
                   "aggr_finish_"
                << level << "_out));" << std::endl;
    }

    finish_condition_ss << "do_hash_aggr_finish_" << level;

    codegen_ctx->function_list.push_back(action_list_define_function_ss.str());
    codegen_ctx->prepare_codes += prepare_ss.str();
    codegen_ctx->process_codes += process_ss.str();
    codegen_ctx->definition_codes += define_ss.str();
    codegen_ctx->aggregate_prepare_codes += aggr_prepare_ss.str();
    codegen_ctx->aggregate_finish_codes += finish_ss.str();
    codegen_ctx->aggregate_finish_condition_codes += finish_condition_ss.str();
    // set join output list for next kernel.
    ///////////////////////////////
    *codegen_ctx_out = codegen_ctx;

    return arrow::Status::OK();
  }

 private:
  arrow::compute::FunctionContext* ctx_;
  arrow::MemoryPool* pool_;
  std::string signature_;

  std::vector<std::shared_ptr<arrow::Field>> input_field_list_;
  std::vector<std::shared_ptr<gandiva::Node>> action_list_;
  std::vector<std::shared_ptr<arrow::Field>> result_field_list_;
  std::vector<std::shared_ptr<gandiva::Node>> result_expr_list_;
};

arrow::Status HashAggregateKernel::Make(
    arrow::compute::FunctionContext* ctx,
    std::vector<std::shared_ptr<gandiva::Node>> input_field_list,
    std::vector<std::shared_ptr<gandiva::Node>> action_list,
    std::vector<std::shared_ptr<gandiva::Node>> result_field_node_list,
    std::vector<std::shared_ptr<gandiva::Node>> result_expr_node_list,
    std::shared_ptr<KernalBase>* out) {
  *out = std::make_shared<HashAggregateKernel>(
      ctx, input_field_list, action_list, result_field_node_list, result_expr_node_list);
  return arrow::Status::OK();
}

HashAggregateKernel::HashAggregateKernel(
    arrow::compute::FunctionContext* ctx,
    std::vector<std::shared_ptr<gandiva::Node>> input_field_list,
    std::vector<std::shared_ptr<gandiva::Node>> action_list,
    std::vector<std::shared_ptr<gandiva::Node>> result_field_node_list,
    std::vector<std::shared_ptr<gandiva::Node>> result_expr_node_list) {
  impl_.reset(new Impl(ctx, input_field_list, action_list, result_field_node_list,
                       result_expr_node_list));
  kernel_name_ = "HashAggregateKernelKernel";
}
#undef PROCESS_SUPPORTED_TYPES

arrow::Status HashAggregateKernel::MakeResultIterator(
    std::shared_ptr<arrow::Schema> schema,
    std::shared_ptr<ResultIterator<arrow::RecordBatch>>* out) {
  return impl_->MakeResultIterator(schema, out);
}

arrow::Status HashAggregateKernel::DoCodeGen(
    int level,
    std::vector<std::pair<std::pair<std::string, std::string>, gandiva::DataTypePtr>>
        input,
    std::shared_ptr<CodeGenContext>* codegen_ctx_out, int* var_id) {
  return impl_->DoCodeGen(level, input, codegen_ctx_out, var_id);
}

std::string HashAggregateKernel::GetSignature() { return impl_->GetSignature(); }

}  // namespace extra
}  // namespace arrowcompute
}  // namespace codegen
}  // namespace sparkcolumnarplugin
