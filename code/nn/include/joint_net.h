#ifndef JOINT_NET_H
#define JOINT_NET_H

#include "inet.h"

template<MatMode mode, typename Dtype>
class JointNet : public INet<mode, Dtype>
{
public:

	JointNet(IEventTimeLoader<mode>* _etloader) : INet<mode, Dtype>(_etloader) {}

	virtual void LinkTrainData() override 
	{
    	this->train_feat["last_hidden"] = this->g_last_hidden_train;
    	for (unsigned i = 0; i < cfg::bptt; ++i)
    	{        
    		this->train_feat[fmt::sprintf("event_input_%d", i)] = this->g_event_input[i];
        	this->train_label[fmt::sprintf("nll_%d", i)] = this->g_event_label[i];
        	this->train_feat[fmt::sprintf("time_input_%d", i)] = this->g_time_input[i];
        	this->train_label[fmt::sprintf("mse_%d", i)] = this->g_time_label[i];
        	this->train_label[fmt::sprintf("mae_%d", i)] = this->g_time_label[i];
            this->train_label[fmt::sprintf("err_cnt_%d", i)] = this->g_event_label[i];
            if (cfg::loss_type == LossType::EXP)
                this->train_label[fmt::sprintf("expnll_%d", i)] = this->g_time_label[i];
    	}
	}

	virtual void LinkTestData() override
	{
		this->test_feat["last_hidden"] = this->g_last_hidden_test;
		this->test_feat["event_input_0"] = this->g_event_input[0];
		this->test_feat["time_input_0"] = this->g_time_input[0];
		this->test_label["mse_0"] = this->g_time_label[0];
		this->test_label["mae_0"] = this->g_time_label[0];
		this->test_label["nll_0"] = this->g_event_label[0];
        this->test_label["err_cnt_0"] = this->g_event_label[0];
        if (cfg::loss_type == LossType::EXP)
            this->test_label["expnll_0"] = this->g_time_label[0];
	}

	virtual void PrintTrainBatchResults(std::map<std::string, Dtype>& loss_map) 
	{
		Dtype rmse = 0.0, mae = 0.0, nll = 0.0, err_cnt = 0.0, expnll = 0;
		for (unsigned i = 0; i < cfg::bptt; ++i)
        {
            mae += loss_map[fmt::sprintf("mae_%d", i)];
            rmse += loss_map[fmt::sprintf("mse_%d", i)];
        	nll += loss_map[fmt::sprintf("nll_%d", i)]; 
            err_cnt += loss_map[fmt::sprintf("err_cnt_%d", i)]; 
            if (cfg::loss_type == LossType::EXP)
                expnll += loss_map[fmt::sprintf("expnll_%d", i)];  
        }
        rmse = sqrt(rmse / cfg::bptt / cfg::batch_size);
		mae /= cfg::bptt * cfg::batch_size;
		nll /= cfg::bptt * train_data->batch_size;
        err_cnt /= cfg::bptt * train_data->batch_size;
        expnll /= cfg::bptt * cfg::batch_size;
        std::cerr << fmt::sprintf("train iter=%d\tmae: %.4f\trmse: %.4f\tnll: %.4f\terr_rate: %.4f", cfg::iter, mae, rmse, nll, err_cnt);
        if (cfg::loss_type == LossType::EXP)
            std::cerr << fmt::sprintf("\texpnll: %.4f", expnll);
        std::cerr << std::endl;
	}

	virtual void PrintTestResults(DataLoader<TEST>* dataset, std::map<std::string, Dtype>& loss_map) 
	{
		Dtype rmse = loss_map["mse_0"], mae = loss_map["mae_0"], nll = loss_map["nll_0"];
		rmse = sqrt(rmse / dataset->num_samples);
		mae /= dataset->num_samples;
		nll /= dataset->num_samples;
        Dtype err_cnt = loss_map["err_cnt_0"] / dataset->num_samples;
        std::cerr << fmt::sprintf("test_mae: %.4f\ttest_rmse: %.4f\ttest_nll: %.4f\ttest_err_rate: %.4f", mae, rmse, nll, err_cnt);
        if (cfg::loss_type == LossType::EXP)
            std::cerr << fmt::sprintf("\texpnll: %.4f", loss_map["expnll_0"] / dataset->num_samples);
        std::cerr << std::endl;        
	}

	virtual void InitParamDict() 
	{
		this->param_dict["w_embed"] = new LinearParam<mode, Dtype>("w_embed",  train_data->num_events, cfg::n_embed, 0, cfg::w_scale);
    	this->param_dict["w_event2h"] = new LinearParam<mode, Dtype>("w_event2h", cfg::n_embed, cfg::n_hidden, 0, cfg::w_scale);
		this->param_dict["w_time2h"] = new LinearParam<mode, Dtype>("w_time2h", cfg::time_dim, cfg::n_hidden, 0, cfg::w_scale);
    	this->param_dict["w_h2h"] = new LinearParam<mode, Dtype>("w_h2h", cfg::n_hidden, cfg::n_hidden, 0, cfg::w_scale);

        unsigned hidden_size = cfg::n_hidden;
        if (cfg::n_h2)
        {
            hidden_size = cfg::n_h2;
            this->param_dict["w_hidden2"] = new LinearParam<mode, Dtype>("w_hidden2", cfg::n_hidden, cfg::n_h2, 0, cfg::w_scale);
        }
        this->param_dict["w_event_out"] = new LinearParam<mode, Dtype>("w_event_out", hidden_size, train_data->num_events, 0, cfg::w_scale);
    	this->param_dict["w_time_out"] = new LinearParam<mode, Dtype>("w_time_out", hidden_size, 1, 0, cfg::w_scale);
	}

	virtual ILayer<mode, Dtype>* AddNetBlocks(int time_step, 
											  GraphNN<mode, Dtype>& gnn, 
											  ILayer<mode, Dtype> *last_hidden_layer, 
                                    		  std::map< std::string, IParam<mode, Dtype>* >& param_dict)
	{
    	gnn.AddLayer(last_hidden_layer);
    	auto* event_input_layer = new InputLayer<mode, Dtype>(fmt::sprintf("event_input_%d", time_step), GraphAtt::NODE);
    	auto* time_input_layer = new InputLayer<mode, Dtype>(fmt::sprintf("time_input_%d", time_step), GraphAtt::NODE);

    	auto* embed_layer = new SingleParamNodeLayer<mode, Dtype>(fmt::sprintf("embed_%d", time_step), param_dict["w_embed"], GraphAtt::NODE); 

    	auto* relu_embed_layer = new ReLULayer<mode, Dtype>(fmt::sprintf("relu_embed_%d", time_step), GraphAtt::NODE, WriteType::INPLACE);

    	auto* hidden_layer = new NodeLayer<mode, Dtype>(fmt::sprintf("hidden_%d", time_step));
    	hidden_layer->AddParam(time_input_layer->name, param_dict["w_time2h"], GraphAtt::NODE); 
    	hidden_layer->AddParam(relu_embed_layer->name, param_dict["w_event2h"], GraphAtt::NODE); 
    	hidden_layer->AddParam(last_hidden_layer->name, param_dict["w_h2h"], GraphAtt::NODE); 

    	auto* relu_hidden_layer = new ReLULayer<mode, Dtype>(fmt::sprintf("relu_hidden_%d", time_step), GraphAtt::NODE, WriteType::INPLACE);
    	auto* event_output_layer = new SingleParamNodeLayer<mode, Dtype>(fmt::sprintf("event_out_%d", time_step), param_dict["w_event_out"], GraphAtt::NODE); 

    	auto* time_out_layer = new SingleParamNodeLayer<mode, Dtype>(fmt::sprintf("time_out_%d", time_step), param_dict["w_time_out"], GraphAtt::NODE); 
    	//auto* exp_layer = new ExpLayer<mode, Dtype>(fmt::sprintf("expact_%d", time_step), GraphAtt::NODE, WriteType::INPLACE);

    	auto* classnll = new ClassNLLCriterionLayer<mode, Dtype>(fmt::sprintf("nll_%d", time_step), true);
    	auto* mse_criterion = new MSECriterionLayer<mode, Dtype>(fmt::sprintf("mse_%d", time_step), 
                                                                 cfg::lambda, 
                                                                 cfg::loss_type == LossType::MSE ? PropErr::T : PropErr::N);
    	auto* mae_criterion = new ABSCriterionLayer<mode, Dtype>(fmt::sprintf("mae_%d", time_step), PropErr::N);
        auto* err_cnt = new ErrCntCriterionLayer<mode, Dtype>(fmt::sprintf("err_cnt_%d", time_step));

    	gnn.AddEdge(event_input_layer, embed_layer);
    	gnn.AddEdge(embed_layer, relu_embed_layer);
    	
    	gnn.AddEdge(time_input_layer, hidden_layer);
    	gnn.AddEdge(relu_embed_layer, hidden_layer);
    	gnn.AddEdge(last_hidden_layer, hidden_layer);
    
    	gnn.AddEdge(hidden_layer, relu_hidden_layer);
    	
        auto* top_hidden = relu_hidden_layer;
        if (cfg::n_h2)
        {
            auto* hidden_2 = new SingleParamNodeLayer<mode, Dtype>(fmt::sprintf("hidden_2_%d", time_step), param_dict["w_hidden2"], GraphAtt::NODE);
            gnn.AddEdge(relu_hidden_layer, hidden_2);
            auto* relu_2 = new ReLULayer<mode, Dtype>(fmt::sprintf("relu_h2_%d", time_step), GraphAtt::NODE, WriteType::INPLACE);
            gnn.AddEdge(hidden_2, relu_2);
            top_hidden = relu_2;
        } 

        gnn.AddEdge(top_hidden, event_output_layer);
        gnn.AddEdge(top_hidden, time_out_layer);    
    
    	//gnn.AddEdge(time_out_layer, exp_layer);
    	gnn.AddEdge(time_out_layer, mse_criterion);
    	gnn.AddEdge(time_out_layer, mae_criterion);
        if (cfg::loss_type == LossType::EXP)
        {
            auto* expnll_criterion = new ExpNLLCriterionLayer<mode, Dtype>(fmt::sprintf("expnll_%d", time_step));
            gnn.AddEdge(time_out_layer, expnll_criterion); 
        }
    	
    	gnn.AddEdge(event_output_layer, classnll);
        gnn.AddEdge(event_output_layer, err_cnt); 

		return relu_hidden_layer; 
	}

    virtual void WriteTestBatch(FILE* fid) override
    {
        this->net_test.GetDenseNodeState("time_out_0", time_pred);
        this->net_test.GetDenseNodeState("event_out_0", event_pred);
        for (size_t i = 0; i < time_pred.rows; ++i)
        {
            fprintf(fid, "%.6f ", time_pred.data[i]);
            int pred = 0; 
            Dtype best = event_pred.data[i * event_pred.cols];
            for (size_t j = 1; j < event_pred.cols; ++j)
                if (event_pred.data[i * event_pred.cols + j] > best)
                {
                    best = event_pred.data[i * event_pred.cols + j]; 
                    pred = j;
                }
            fprintf(fid, "%d\n", pred);
        }
    }

    DenseMat<CPU, Dtype> time_pred, event_pred;
};

#endif