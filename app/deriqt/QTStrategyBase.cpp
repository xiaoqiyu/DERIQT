#include <iostream>
#include "QTStrategyBase.h"

bool cmp_max(std::pair<std::string, double> x, std::pair<std::string, double>y)
{
	return x.second > y.second;
}
void cache_main_instruments(std::vector<std::string> _v_instrument_id);

int QTStrategyBase::init(std::vector<std::string>&  _v_product_ids, const std::string _conf_file_name)
{
	//初始
	LOG(WARNING)<<"[Init] Init Strategy with mode: "<<this->mode<< "conf file: "<<_conf_file_name;

	FileName _conf_file = {'\0'};
	strcpy(_conf_file, _conf_file_name.c_str());
	INIReader reader(_conf_file);
	if (reader.ParseError() != 0)
	{
		LOG(FATAL)<< "[init] Fail to load config file in current directory:"<< _conf_file;
		return 1;
	}
	
	FileName _strategy_file = {'\0'};
	std::string _str_file_name = "/home/kiki/workspace/CTPTrader/conf/"+this->name+".ini"; //FIXME remove hardcode for the conf path
	LOG(WARNING)<<"[Init]read strategy conf name:"<<_str_file_name;
	strcpy(_strategy_file, _str_file_name.c_str());
	INIReader reader_str(_strategy_file);
	if (reader_str.ParseError() != 0)
	{
		LOG(FATAL)<< "[init] Fail to load config file in current directory:"<< _str_file_name;
		return 1;
	}

	conf_signal_counter = std::stoi(reader_str.Get("strategy","signal_interval","0"));
	p_factor = new Factor(std::stoi(reader_str.Get("strategy","long_windows","0")), std::stoi(reader_str.Get("strategy","short_windows","0")));


	std::string _cache_file_name = "/home/kiki/workspace/CTPTrader/conf/daily_cache.ini"; //FIXME remove hardcode for the conf path
	INIReader reader_daily_cache(_cache_file_name);
	if (reader_daily_cache.ParseError() != 0)
	{
		LOG(FATAL)<< "[init] Fail to load config file in current directory:"<< _cache_file_name;
		return 1;
	}
	
	
	char mdAddr[40];
	char ch[40];
	std::string trading_date;

	//init private var
	this->task_tag = _v_product_ids[0]; 
	this->broker_id = reader.Get("user", "BrokerID", "9999");
	this->user_id = reader.Get("user", "UserID", "123456");

	//when the stratey starts, pop and clear the stale depth market since there are cases when the trade process stop but 
	//market process still subscribe the depth market and push factor the shm queue
	//策略启动时，先清空行情队列由行情进程推送的过期数据
	shm::shared_string v_tmp(*char_alloc_ptr);
	while(!p_queue->empty()){
		p_queue->pop(v_tmp);
	}

	LOG(WARNING)<<"***************[Init]Start CTP Handler Init****************";
	//mode 0(subscribe depth market) and mode 2(live trade) need to connect to ctp td; mode 0 for query api, mode 2 for order api 
	//CTP Trader handler init
	this->p_trader_handler = new CTPTraderHandler();
	this->p_trader_handler->CreateFtdcTraderApi();
	this->p_trader_handler->RegisterFront(strcpy(ch, reader.Get("td", "FrontAddr", "127.0.0.1:1234").c_str()));
	this->p_trader_handler->init(task_tag, broker_id, user_id);
	sleep(5);

	LOG(WARNING) << "[Init] Start CTP Authenticate.......";
	CThostFtdcReqAuthenticateField reqAuth = {0};
	strcpy(reqAuth.BrokerID, reader.Get("user", "BrokerID", "9999").c_str());
	strcpy(reqAuth.UserID, reader.Get("user", "UserID", "123456").c_str());
	strcpy(reqAuth.AuthCode, reader.Get("user", "AuthCode", "!@#$%^&*").c_str());
	strcpy(reqAuth.AppID, reader.Get("user", "AppID", "MyProgram").c_str());
	this->p_trader_handler->ReqAuthenticate(&reqAuth, nRequestID++);
	sleep(5);

	LOG(WARNING)<< "[Init] Start CTP Login......";
	CThostFtdcReqUserLoginField reqUserLogin = {0};
	strcpy(reqUserLogin.BrokerID, reader.Get("user", "BrokerID", "9999").c_str());
	strcpy(reqUserLogin.UserID, reader.Get("user", "UserID", "123456").c_str());
	strcpy(reqUserLogin.Password, reader.Get("user", "Password", "123456").c_str());
	strcpy(reqUserLogin.MacAddress, reader.Get("user", "MacAddress", "123456").c_str());
	strcpy(reqUserLogin.UserProductInfo, reader.Get("user", "UserProductInfo", "123456").c_str());
	this->p_trader_handler->ReqUserLogin(&reqUserLogin, nRequestID++);
	sleep(5);

	trading_date = this->p_trader_handler->getTradingDay();
	LOG(WARNING)<< "[Init] CTP log in sucess, Trading date is: " << trading_date;

	LOG(WARNING)<<"[Init] Start query and init local position table...";
	this->p_trader_handler->init_positions(this->user_id, this->broker_id);
	LOG(WARNING)<<"[Init] End query and init local position table...";
	

	LOG(WARNING)<<"[Init] Start Cache main instruments.....";
	std::cout<<"[Init] Start Cache main instruments....."<<std::endl;
	this->cache_main_instruments(_v_product_ids);
	LOG(WARNING)<<"[Init] End Cache main instruments.....";
	std::cout<<"[Init] End Cache main instruments....."<<std::endl;


	if (strategy_class == 0){
		for(auto it=v_main_contract_ids.begin(); it!=v_main_contract_ids.end();++it)
		{
			LOG(WARNING)<<"[Init] strategy class=>0, push main future contract id to target v_instrumentID=>"<<(*it);
			this->v_instrummentID.push_back((*it));
		}

	}else if(strategy_class == 1){
		for(auto it=v_option_ids.begin(); it!=v_option_ids.end();++it)
		{
			LOG(WARNING)<<"[Init] strategy class=>1, push option id to target v_instrumentID=>"<<(*it);
			this->v_instrummentID.push_back((*it));
		}
	}else{
		LOG(WARNING)<<"[Init] unhandle strategy class, strategy_class=>"<<strategy_class;
	}
	LOG(WARNING)<<"***************[Init] End CTP Handler Init****************";


	LOG(WARNING)<<"************** [Init] Start cache daily market*************";

	//从当天存下的行情记录中找到第一个tick作为open price,因为daily cache的接口没有当天的行情
	std::ofstream * cache_ptr = new std::ofstream();
	FileName _cache_filename = {'\0'};
	//FIXME remove hardcode the cache path
	sprintf(_cache_filename, "/home/kiki/workspace/CTPTrader/cache/mkt/%s_depth_market_data_%s.recordio", this->task_tag.c_str(), trading_date.c_str());
	std::ifstream ifs(_cache_filename, std::ios::binary);
    recordio::RecordReader reader1(&ifs);
	CThostFtdcDepthMarketDataField *p_mkt = new CThostFtdcDepthMarketDataField();
	std::string buffer;
	while (reader1.ReadBuffer(buffer))
    {
        assert(buffer.size()==sizeof(CThostFtdcDepthMarketDataField));
        memcpy(p_mkt, buffer.data(), buffer.size());
		// std::cout<<p_mkt->InstrumentID<<","<<p_mkt->UpdateTime<<","<<p_mkt->LastPrice<<std::endl;
		for (auto it = this->v_instrummentID.begin(); it != this->v_instrummentID.end(); ++it){
			std::string curr_date = p_mkt->TradingDay;
			std::string curr_ts = p_mkt->UpdateTime;
			std::string cache_instrument_id = *it;
			if(cache_instrument_id ==  p_mkt->InstrumentID){
				auto it1 = this->m_daily_cache.find(*it);
				if(it1 == this->m_daily_cache.end()){ //只有找不到相关的合约才做一次初始化，就保证了每个合约的open price只用了第一个深度行情数�?
					if(curr_date==trading_date){
						ptr_daily_cache _tmp_ptr = new daily_cache(); //FIXME add delete
						_tmp_ptr->InstrumentID = cache_instrument_id;
						_tmp_ptr->open_price = p_mkt->LastPrice;
						m_daily_cache.insert(std::pair<std::string, ptr_daily_cache>(cache_instrument_id, _tmp_ptr));
						LOG(WARNING)<<"[Init] Got cache open price for instrument id:"<<*it<<":update time:"<<p_mkt->UpdateTime<<", open:"<<p_mkt->LastPrice;
					}
				}
			}
		}
    }
    reader1.Close();

	//daily cache, 盘前日频行情初始化，从daily_ini中读取各个合约的日频行情进行初始化
	std::set<std::string> cache_sesstions = reader_daily_cache.Sections();
	for (const auto &sec : cache_sesstions) 
	{     
		auto it = this->m_daily_cache.find(sec);
		if (it != this->m_daily_cache.end()){ //instrument id exist
			ptr_daily_cache _tmp_ptr =it->second;
			_tmp_ptr->hh = std::stod(reader_daily_cache.Get(sec, "hh","0.0"));
			_tmp_ptr->hc = std::stod(reader_daily_cache.Get(sec, "hc","0.0"));
			_tmp_ptr->ll = std::stod(reader_daily_cache.Get(sec, "ll","0.0"));
			_tmp_ptr->lc = std::stod(reader_daily_cache.Get(sec, "lc","0.0"));
			_tmp_ptr->up_limit = std::stod(reader_daily_cache.Get(sec, "up_limit_price","0.0"));
			_tmp_ptr->down_limit = std::stod(reader_daily_cache.Get(sec, "down_limit_price","0.0"));
			_tmp_ptr->product_id = reader_daily_cache.Get(sec, "product_id","");
		}else{
			ptr_daily_cache _tmp_ptr = new daily_cache();
			_tmp_ptr->hh = std::stod(reader_daily_cache.Get(sec, "hh","0.0"));
			_tmp_ptr->hc = std::stod(reader_daily_cache.Get(sec, "hc","0.0"));
			_tmp_ptr->ll = std::stod(reader_daily_cache.Get(sec, "ll","0.0"));
			_tmp_ptr->lc = std::stod(reader_daily_cache.Get(sec, "lc","0.0"));
			_tmp_ptr->up_limit = std::stod(reader_daily_cache.Get(sec, "up_limit_price","0.0"));
			_tmp_ptr->down_limit = std::stod(reader_daily_cache.Get(sec, "down_limit_price","0.0"));
			_tmp_ptr->product_id = reader_daily_cache.Get(sec, "product_id","");
			m_daily_cache.insert(std::pair<std::string, ptr_daily_cache>(sec, _tmp_ptr));
		}
	}//end of session loop
	LOG(WARNING)<<"**************[Init] End cache daily market*************";

	ptr_daily_cache p_curr_daily_cache = NULL;
	for (const auto &it : m_daily_cache){
		if(it.second->product_id == this->task_tag){
			p_curr_daily_cache = it.second; //TODO to be tested
		}
	} 
	this->p_trader_handler->init_price_limits(p_curr_daily_cache);
	
	if(this->mode == 0){//mode 0 need to connect CTPMD to subscribe depth market data
		LOG(WARNING)<<"***************[Init] Start CTP MD Init****************";
		//CTP MD connect and init
		this->p_md_handler = new CTPMdHandler();
		this->p_md_handler->set_config(_conf_file);
		this->p_md_handler->CreateFtdcMdApi();
		this->p_md_handler->RegisterFront(strcpy(mdAddr, reader.Get("md", "FrontMdAddr", "127.0.0.1:1234").c_str()));
		this->p_md_handler->init(this->v_instrummentID);
		LOG(WARNING)<<"***************[Init] End CTP MD Init****************";
	}

	// if(this->mode == 1){//mode 1 will connet to simtrade server for simulation
	// 	LOG(WARNING)<<"[Init] Mode 1: init gm simtrade.....";
	// 	simtrade_token = reader.Get("simtrade", "token", "a1128cf0aaa3735b04a2706c8029a562e8c2c6b6");
	// 	simtrade_account_id = reader.Get("simtrade", "account_id", "a1a91403-2fc2-11ec-bd15-00163e0a4100");
	// 	simtrade_ptr.reset(new SimTrader(reader.Get("simtrade", "token", "a1128cf0aaa3735b04a2706c8029a562e8c2c6b6").c_str()));
	// 	// 设置服务地址api.myquant.cn:9000
	// 	simtrade_ptr->set_endpoint ( reader.Get("simtrade", "endpoint", "api.myquant.cn:9000").c_str());
	// 	std::string future_acc = reader.Get("simtrade", "account_id", "a1a91403-2fc2-11ec-bd15-00163e0a4100");		
	// 	simtrade_account_id = future_acc;
		
	// 	// 登录账户id
	// 	simtrade_ptr->login(future_acc.c_str());
	// 	// simtrade_ptr->init_positions(future_acc);
	// 	//开始接收事�?
	// 	int status = simtrade_ptr->start();
	// 	//判断状�?
	// 	if (status == 0)
	// 	{
	// 	    LOG(WARNING) << "[Init] Connected to simtrade server" << std::endl;
	// 	}
	// 	else
	// 	{
	// 	    LOG(ERROR) << "[Init] Fail to connect to simtrade server" << std::endl;
	// 	}
	// }

	LOG(WARNING)<<"***************[Init] Start Resource(queue/thread) Allocation****************";
	//data/order thread init,队列和线程资源分配
	if (this->mode == 0){ // start the data thread to process depth markets
		LOG(WARNING)<<"[Init] Mode 0: Create thread for data thread";
		this->data_thread = thread(&QTStrategyBase::on_tick, this);
		//mode 0 for kline resample and depth market cache
	    LOG(WARNING)<<"[Init] Mode 0: Create cache writer";
		int cnt = 0;
		//private varilbe init
		for(auto iter = this->v_instrummentID.begin(); iter!=this->v_instrummentID.end(); ++iter)
		{
			TickToKlineHelper *p_kline_helper =  new TickToKlineHelper();
			v_t2k_helper.push_back(p_kline_helper);
			cnt ++;
		}
		std::ofstream * p_depth_mkt = new std::ofstream();
		FileName _depth_mkt_filename = {'\0'};
		sprintf(_depth_mkt_filename, "cache/mkt/%s_depth_market_data_%s.recordio", this->task_tag.c_str(), trading_date.c_str());
		p_depth_mkt->open(_depth_mkt_filename, std::ios::app|std::ios::binary);
		this->p_depth_mkt_writer = new recordio::RecordWriter(p_depth_mkt);									
	}else if (this-> mode == 1 || this->mode ==2){
		LOG(WARNING)<<"[Init] Mode 1&2: create order/risk/signal data queue.......";
		// order data queue for sim/live trade
		this->p_order_queue = new DataQueue();
		this->p_risk_queue = new DataQueue();
		this->p_sig = new OrderSignal(this->name);

		LOG(WARNING)<<"[Init] Mode 1&2: create and init strategyconfig......";
		this->p_strategy_config = new StrategyConfig(); //new and init the strategy config
		p_strategy_config->close_type = std::stoi(reader_str.Get("strategy","close_type","0"));
		p_strategy_config->stop_profit = std::stod(reader_str.Get("strategy", "stop_profit","1"));
		p_strategy_config->stop_loss = std::stod(reader_str.Get("strategy", "stop_loss","1"));
		p_strategy_config->vol_limit = std::stoi(reader_str.Get("strategy", "vol_limit","1"));
		p_strategy_config->init_cash = std::stod(reader_str.Get("strategy", "init_cash","1000000"));
		p_strategy_config->risk_ratio = std::stod(reader_str.Get("strategy", "risk_ratio","1000000"));
		p_strategy_config->signal_delay = std::stoi(reader_str.Get("strategy", "signal_delay","5"));
		p_strategy_config->signal_interval = std::stoi(reader_str.Get("strategy", "signal_interval","5"));
		p_strategy_config->risk_duration = std::stoi(reader_str.Get("strategy", "risk_duration","60"));
		p_strategy_config->cancel_order_delay = std::stoi(reader_str.Get("strategy", "cancel_order_delay","120"));
		LOG(WARNING)<<"stop profit(in price)=>"<<p_strategy_config->stop_profit<<", stop loss(in price)=>"<<p_strategy_config->stop_loss<<", vol limit=>"<<p_strategy_config->vol_limit
		<<", init cash=>"<<p_strategy_config->init_cash<<", risk ratio=>"<<p_strategy_config->risk_ratio
		<<", signal delay=>"<<p_strategy_config->signal_delay<<"(sec), risk duration=>"<<p_strategy_config->risk_duration<<"(sec), cancel order delay=>"<<p_strategy_config->cancel_order_delay<<"(sec), signal interval=>"<<p_strategy_config->signal_interval<<"(tick)";
	}else{
		LOG(ERROR)<< "Invalid mode for strategy";
	}
	this->active_ = true;
	LOG(WARNING)<<"***************[Init] End Resource(queue/thread) Allocation****************";
	return 0;
};

void QTStrategyBase::on_event()
{
	LOG(WARNING)<<"[on_event] Start process signal: on event";
	try
	{
		bool _terminate = false;  //控制本循环的本地变量
		while(!_terminate) 
		{
			shm::shared_string v(*char_alloc_ptr);
			std::time_t now_time = std::time(nullptr);
    		if (p_queue->pop(v)) 
			{
				std::string msg = v.data();
				// LOG(WARNING)<<"[on_event] rev msg:"<<msg;
				if (msg== "null\n"){ 
					LOG(WARNING)<<"[on_event] receive null msg from on_tick, set _terminate for on_event, msg=>"<<msg;
					_terminate = true;
					OrderData* p_orderdata = new OrderData();
					p_orderdata->status = TERMINATE_SIGNAL;
					p_orderdata->order_insert_time = now_time;
					place_order(p_orderdata);
				}else{
					std::vector<std::string> v_rev;
					char c = ',';
					std::stringstream sstr(msg);
					std::string token;
					while(getline(sstr, token, c)){
						v_rev.push_back(token);
					}

					if (this->mode == 1 || this->mode == 2){//simtrade/live trade
						std::time_t duration =  now_time - last_order_time;
						//push into risk queue
						RiskInputData * p_risk_data = new RiskInputData();
						p_risk_data->last_price = std::stod(v_rev[4]);
						p_risk_data->update_time = v_rev[1];
						p_risk_data->symbol = v_rev[0];
						p_risk_data->exchangeid = v_rev[12];
						DataField data = DataField();
						data.data_type = RISK_INPUT;
						data._data = p_risk_data;
						this->p_risk_queue->push(data);
						std::string _symbol = v_rev[0];
						auto it_daily = m_daily_cache.find(v_rev[0]);
						ptr_daily_cache p_daily = new daily_cache();
						if (it_daily != m_daily_cache.end()){
							p_daily = it_daily->second; //REMARK any issue?? and if it is the first tick, update open 
						}else{
							p_daily->InstrumentID = v_rev[4];
							p_daily->open_price = std::stod(v_rev[4]); //is not exist in cache, update the open(first tick),ignore the prev date tick
						}
						
						OrderData* p_orderdata = p_sig->get_signal(v_rev, p_daily);
						
						p_orderdata->order_insert_time = now_time;
						if(p_orderdata->status == LONG_SIGNAL || p_orderdata->status==SHORT_SIGNAL){
							LOG(WARNING)<<"[on_event] get_signal:"<<p_orderdata->status<<std::endl;
							place_order(p_orderdata);
						}//end of handle signal
					}//end of handle trade mode
				}//end of input risk and handle signal
			}//end of handle pop
			//if get the terminate signal, stop the whle and join the thread
			if(_terminate)
			{
				LOG(WARNING)<<"[on_event] break on_event";
				break;
			}
		}//end of while
	}
	catch(const std::exception& e)
	{
		LOG(ERROR)<< e.what() << '\n';
	}
	
}//end of on_event


void QTStrategyBase::on_risk()
{
	LOG(WARNING)<<"[on_risk] Start risk thread";
	try{
		bool _terminate = false;
		while(!_terminate){
			DataField data = this->p_risk_queue->pop();
			if(data.data_type==RISK_INPUT && data._data){
				RiskInputData *p_risk_input = reinterpret_cast<RiskInputData *>(data._data);
				std::string _update_time = p_risk_input->update_time;
				double _last_price = p_risk_input->last_price;
				std::time_t now_time = std::time(nullptr);
    			tm *ltm = localtime(&now_time);
				// int update_min = std::stoi(_update_time.substr(3,2));
				// int update_sec = std::stoi(_update_time.substr(6,2));
				if (ltm->tm_hour == 15 && ltm->tm_min>0){ //after one min of market close, it will terminate
					_terminate = true;
				}
				int _risk_monitor = (ltm->tm_min*60 + ltm->tm_sec)%p_strategy_config->risk_duration;
				if(_risk_monitor == 0){//will call risk monitor to check
					// LOG(WARNING)<<"[on_risk] calling risk monitor for update time=>"<<_update_time;
					int ret = this->risk_monitor(p_risk_input, p_strategy_config);
					if (ret== -1) {_terminate=true;}
				}
			}
			if (_terminate){
				LOG(WARNING)<<"[on_risk] break on_risk";
				break;
			}
		}
	}
	catch (const TerminatedError &){
	}
}


void QTStrategyBase::on_tick()
{
	try
	{
		bool _terminate = false; //控制本循环的私有变量
		bool _sent_terminate = false;
		while (!_terminate) 
		{
			DataField data = this->p_md_handler->get_data_queue()->pop();
			char s[factor_len];
			switch (data.data_type)
			{
			case FDEPTHMKT: //期货期权深度行情数据
			{
				if (data._data)
				{
					CThostFtdcDepthMarketDataField *pDepthMarketData = reinterpret_cast<CThostFtdcDepthMarketDataField *>(data._data);
					// std::cout<<"rev in on_tick:"<<pDepthMarketData->UpdateTime<<std::endl;
					bool ret_write_buffer = this->p_depth_mkt_writer->WriteBuffer(reinterpret_cast<const char*>(pDepthMarketData), sizeof(CThostFtdcDepthMarketDataField));
					//FIXME remove hardcode of the timestampe, eod for trade, but still cache depth market
					if (std::strcmp(pDepthMarketData->UpdateTime, "14:55:00") >=0  && std::strcmp(pDepthMarketData->UpdateTime, "21:00:00") <0){
					// if (std::strcmp(pDepthMarketData->UpdateTime, "22:50:00") >=0 ){
						if (!_sent_terminate){
							LOG(WARNING)<<"[on_tick] send null msg to p_queue(to trade process) for time=>"<<pDepthMarketData->UpdateTime;
							std::strcpy(s, "null\n");//TODO remove hardcode msg
							p_queue->push(shm::shared_string(s, *char_alloc_ptr));
							// LOG(WARNING)<<"Break strategy when eod, but still subscribe depth market";
							_sent_terminate = true;
						}
						break;
					}
					//收盘退出on_tick线程
					if (std::strcmp(pDepthMarketData->UpdateTime, "15:00:30") >=0  && std::strcmp(pDepthMarketData->UpdateTime, "21:00:00") <0){
						_terminate = true;
						break;
					}
					//正常处理行情
					bool _process_future = (this->strategy_class == 0) && (find(v_main_contract_ids.begin(), v_main_contract_ids.end(), pDepthMarketData->InstrumentID)!=v_main_contract_ids.end());
					bool _process_option = (this->strategy_class == 1) && (find(v_option_ids.begin(), v_option_ids.end(), pDepthMarketData->InstrumentID)!=v_option_ids.end());
					if(_process_future || _process_option){
						int signal_flag = this->signal_interval % conf_signal_counter; 
						long _offset = p_factor->update_factor(pDepthMarketData, s, signal_flag);	
						if (_offset > 0){
							// LOG(WARNING)<<"Push signal with signal delay=>"<<this->signal_delay<<"offset=>"<<_offset<<",delay flag=>"<<signal_flag<<std::endl;
							auto it = m_product_exchangeid.find(pDepthMarketData->InstrumentID);
							if(it != m_product_exchangeid.end()){
								_offset += sprintf(s + _offset, "%s,", (it->second).c_str());
							}
							p_queue->push(shm::shared_string(s, *char_alloc_ptr));
						}
					}
					//TODO calculate K line
					this->signal_interval += 1;
					delete pDepthMarketData;
				}
				if (data.error)
				{
					// delete data.error;
					LOG(ERROR)<<"ERROR msg in mkt data subscribe";
				}
				break;
			}
			case INVALID_MSG: //end of market
			{
				std::strcpy(s, "null\n");//TODO remove hardcode msg
				p_queue->push(shm::shared_string(s, *char_alloc_ptr));
				_terminate = true;
				LOG(WARNING)<<"Break on_tick, end of subscribe market";
				break;
			}

			default:
				break;
			} //end swtich
			//if got the invalied data type, break the while and join the thread
			if (_terminate)
			{
				LOG(WARNING)<<"[on_tick] break on_tick";
				break; 
			}
		}//end while

	}
	catch (const TerminatedError &)
	{
	}
}

void QTStrategyBase::calculate_kline(){};


void QTStrategyBase::start()
{
	this->start_ = true;
	if(this->mode == 0){
		LOG(WARNING)<<"[start] mode 0: start subscribe mkt data";
		this->p_md_handler->SubscribeMarketData();
		//FIXME exit ctp TD in md
		p_trader_handler->exit();
		
	}else if(this->mode == 1 || this->mode == 2){
		LOG(WARNING)<<"[start] mode 1&2: Simtrade, listening to factor";
		this->signal_thread = thread(&QTStrategyBase::on_event, this); 
		this->order_thread = thread(&QTStrategyBase::process_order, this);
		this->risk_monitor_thread = thread(&QTStrategyBase::on_risk, this);
	}else{
		LOG(ERROR)<<"[start] invalid mode";
	}
}


void QTStrategyBase::insert_limit_order(TThostFtdcPriceType limit_price, TThostFtdcVolumeType volume, TThostFtdcOrderRefType order_ref,  TThostFtdcDirectionType direction,TThostFtdcInstrumentIDType instrumentID)
{
	CThostFtdcInputOrderField * p_input_order_field = new CThostFtdcInputOrderField();
	strcpy(p_input_order_field->InstrumentID, instrumentID);
	strcpy(p_input_order_field->OrderRef, order_ref);
	p_input_order_field->VolumeTotalOriginal = volume;
	p_input_order_field->LimitPrice = limit_price;
	p_input_order_field->Direction = direction;
	p_input_order_field->OrderPriceType = '2';
	DataField data = DataField();
	data.data_type = ORDERFIELDCTP;
	data._data = p_input_order_field;
	this->p_order_queue->push(data);
	// this->p_trader_handler->ReqOrderInsert(&input_order_field, nRequestID);
}

void QTStrategyBase::insert_market_order(TThostFtdcPriceType limit_price, TThostFtdcVolumeType volume, TThostFtdcOrderRefType order_ref, TThostFtdcOrderPriceTypeType order_price_type, TThostFtdcDirectionType direction, TThostFtdcInstrumentIDType instrumentID)
{
	CThostFtdcInputOrderField * p_input_order_field = new CThostFtdcInputOrderField();
	strcpy(p_input_order_field->InstrumentID, instrumentID);
	strcpy(p_input_order_field->OrderRef, order_ref);
	p_input_order_field->VolumeTotalOriginal = volume;
	p_input_order_field->LimitPrice = limit_price;
	p_input_order_field->Direction = direction;
	p_input_order_field->OrderPriceType = order_price_type;
	DataField data = DataField();
	data.data_type = ORDERFIELDCTP;
	data._data = p_input_order_field;
	this->p_order_queue->push(data);
	// this->p_trader_handler->ReqOrderInsert(&input_order_field, nRequestID);
}

//TODO to be added
void QTStrategyBase::insert_limit_order_gfd(TThostFtdcPriceType limit_price, TThostFtdcVolumeType volume, TThostFtdcOrderRefType order_ref,  TThostFtdcDirectionType direction,TThostFtdcInstrumentIDType instrumentID)
{
	CThostFtdcInputOrderField * p_input_order_field = new CThostFtdcInputOrderField();
	strcpy(p_input_order_field->InstrumentID, instrumentID);
	strcpy(p_input_order_field->OrderRef, order_ref);
	p_input_order_field->VolumeTotalOriginal = volume;
	p_input_order_field->LimitPrice = limit_price;
	p_input_order_field->Direction = direction;
	p_input_order_field->OrderPriceType = '2';

	// strcpy(orderfield.BrokerID, "9999");
	// strcpy(orderfield.InvestorID, "000001");  
	// strcpy(orderfield.ExchangeID, “SHFE�?);
	// strcpy(orderfield.InstrumentID, "au1912");  
	// orderfield.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	// orderfield.Direction = THOST_FTDC_D_Sell;
	// orderfield.LimitPrice = 400.0;
	// orderfield.VolumeTotalOriginal = 10;    
	// orderfield.ContingentCondition = THOST_FTDC_CC_Immediately;  
	// orderfield.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	// orderfield.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;    
	// orderfield.TimeCondition = THOST_FTDC_TC_GFD ;
	// orderfield.VolumeCondition = THOST_FTDC_VC_AV;    
	// orderfield.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;  


	DataField data = DataField();
	data.data_type = ORDERFIELDCTP;
	data._data = p_input_order_field;
	this->p_order_queue->push(data);
	// this->p_trader_handler->ReqOrderInsert(&input_order_field, nRequestID);
}

//TODO to be added
void QTStrategyBase::insert_limit_order_fok(TThostFtdcPriceType limit_price, TThostFtdcVolumeType volume, TThostFtdcOrderRefType order_ref,  TThostFtdcDirectionType direction,TThostFtdcInstrumentIDType instrumentID)
{
	CThostFtdcInputOrderField * p_input_order_field = new CThostFtdcInputOrderField();
	strcpy(p_input_order_field->InstrumentID, instrumentID);
	strcpy(p_input_order_field->OrderRef, order_ref);
	p_input_order_field->VolumeTotalOriginal = volume;
	p_input_order_field->LimitPrice = limit_price;
	p_input_order_field->Direction = direction;
	p_input_order_field->OrderPriceType = '2';

	// strcpy(orderfield.BrokerID, "9999");
	// strcpy(orderfield.InvestorID, "000001");
	// strcpy(orderfield.ExchangeID, “SHFE�?);
	// strcpy(orderfield.InstrumentID, "au1912");  
	// orderfield.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
	// orderfield.Direction = THOST_FTDC_D_Sell;
	// orderfield.LimitPrice = 400.0;
	// orderfield.VolumeTotalOriginal = 10;    
	// orderfield.ContingentCondition = THOST_FTDC_CC_Immediately;  
	// orderfield.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
	// orderfield.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;    
	// orderfield.TimeCondition = THOST_FTDC_TC_IOC;
	// orderfield.VolumeCondition = THOST_FTDC_VC_CV;    
	// orderfield.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;  

	DataField data = DataField();
	data.data_type = ORDERFIELDCTP;
	data._data = p_input_order_field;
	this->p_order_queue->push(data);
	// this->p_trader_handler->ReqOrderInsert(&input_order_field, nRequestID);
}

// void QTStrategyBase::insert_order_sim(OrderData* p_order_data){	
	// LOG(WARNING)<<"[insert_order_sim] in insert order sim, symbol:"<<p_order_data->symbol<<",side:"<<p_order_data->side<<",signal:"<<p_order_data->status;
	// DataField data = DataField();
	// data.data_type = ORDERFIELDSIM;
	// OrderData *_order_data = new OrderData();
	// *_order_data = *p_order_data;
	// data._data = _order_data;
	// this->p_order_queue->push(data);
// }

void QTStrategyBase::place_order(OrderData* p_order_data){
	LOG(WARNING)<<"[place_order] symbol:"<<p_order_data->symbol<<",side:"<<p_order_data->side<<",signal(long:142,short:143,stop:147,terminate:148):"<<p_order_data->status;
	DataField data = DataField();
	
	OrderData *_order_data = new OrderData();
	*_order_data = *p_order_data;
	data._data = _order_data;
	if(this->mode == 1){//gm sim trade
		data.data_type = ORDERFIELDSIM;
		this->p_order_queue->push(data);
	//CTP trade
	}else if(this->mode == 2){
		data.data_type = ORDERFIELDCTP;
		this->p_order_queue->push(data);
	}else{
		LOG(WARNING)<<"[place_order] Invalid mode";
	}
}

std::tuple<std::vector<std::string>, std::vector<std::string>> QTStrategyBase::get_instrument_by_product(std::string product_id)
{
	CThostFtdcQryInstrumentField pQryInstrument = {0};
	std::strcpy(pQryInstrument.InstrumentID, product_id.c_str());
	//TODO check return value
	int ret = this->p_trader_handler->ReqQryInstrument(&pQryInstrument, nRequestID++);
	return std::make_tuple(this->p_trader_handler->GetFutureInstrumentID(),this->p_trader_handler->GetOptionInstrumentID());
	// return this->p_trader_handler->GetFutureInstrumentID();

}

std::vector<CThostFtdcInvestorPositionField *> QTStrategyBase::get_investor_position(std::string investor_id, std::string broker_id)
{
	return p_trader_handler->get_investor_position(investor_id, broker_id);
}

std::vector<CThostFtdcTradingAccountField*> QTStrategyBase::get_account(std::string investor_id, std::string broker_id)
{
	return p_trader_handler->get_account(investor_id, broker_id);
}

int QTStrategyBase::req_trade(std::string investor_id, std::string broker_id)
{
	CThostFtdcQryTradeField trade_fields = {0};
	std::strcpy(trade_fields.InvestorID, investor_id.c_str());
	std::strcpy(trade_fields.BrokerID, broker_id.c_str());
	// int ret_req = this->p_trader_handler->ReqQryTradingAccount(&trade_fields, nRequestID++);	
	int ret_req = this->p_trader_handler->ReqQryTrade(&trade_fields, nRequestID++);
	LOG(WARNING)<<"[req_trade] Req return in req_trade:"<<ret_req;
	return ret_req;
}


int QTStrategyBase::get_position_details(std::string investor_id, std::string broker_id)
{
	CThostFtdcQryInvestorPositionDetailField pos_detail_fields = {0};
	std::strcpy(pos_detail_fields.InvestorID, investor_id.c_str());
	std::strcpy(pos_detail_fields.BrokerID, broker_id.c_str());
	int ret_req = this->p_trader_handler->ReqQryInvestorPositionDetail(&pos_detail_fields,nRequestID++);
	LOG(WARNING)<<"Req return in position details is:"<<ret_req;
	return ret_req;
}

//等待行情和交易线程退出，暂停策略时调�?
void QTStrategyBase::stop()
{
	//TODO stop data_thread, but the ctp instance is still active
	LOG(WARNING)<<"[stop] Calling stop for strategy...";
	if(this->mode == 0){
		LOG(WARNING)<<"[stop] Mode 0: join data thread";
		this->data_thread.join();
	}else if (this->mode == 1 || this->mode == 2){
		LOG(WARNING)<<"[stop] Mode 1&2: join order thread";
		this->signal_thread.join();
		std::cout<<"complete signal thread join"<<std::endl;
		this->order_thread.join();
		std::cout<<"complete order thread join"<<std::endl;
		this->risk_monitor_thread.join();
		std::cout<<"complete risk thread join"<<std::endl;
	}else{
		LOG(ERROR)<<"[stop] Invalid mode in stop:"<<this->mode;
	}
    sleep(3);
	start_ = false;
}

//释放资源,退出应用程序时调用
void QTStrategyBase::release()
{
	LOG(WARNING)<<"[release] Calling release in strategy";
	if (this->mode == 0){
		LOG(WARNING)<<"[release] Mode 0: delete t2k helper";
		LOG(WARNING)<<"[release] Mode 0 in relase:stop CTP MD";
		this->p_md_handler->exit();
	}else if(this->mode == 1 || this->mode==2){
		LOG(WARNING)<<"[release] Mode 1&2: delete order queue in release";
		p_order_queue->terminate();
		delete p_order_queue;
		std::cout<<"complete delete order queue"<<std::endl;
		p_risk_queue->terminate();
		delete p_risk_queue;
		std::cout<<"complete delete p risk queue"<<std::endl;
		delete p_queue;
		std::cout<<"complete delete p queue"<<std::endl;
		this->p_trader_handler->exit();

	}else{
		LOG(WARNING)<<"[release] Invalid mode in relase:"<<this->mode;
	}
	active_ = false;	
}


std::vector<std::string> QTStrategyBase::getInstrumentID()
{
	return this->v_instrummentID;
}

// void QTStrategyBase::setInstrumentID(std::vector<std::string> v_instrumentid)
// {
// 	if (!v_instrummentID.empty())
// 	{
// 		for(auto iter=v_instrummentID.begin(); iter!=v_instrummentID.end(); ++iter)
// 		{
// 			this->v_instrummentID.push_back(*iter);
// 		}
// 	}
// }



/*
verify and update order data for CTP
LONG_SIGNAL:
	- short position: close the positions for all volume  with limit order by stop_profit
	- long position: open  long  positions with market order until vol limit
SHORT_SIGNAL:
	- long position: close the positions for all volume with limit order by stop profit
	- short position: open short positions with market order until vol limit 
STOP_SIGNAL:
	- stop profit and loss order
*/
int QTStrategyBase::verify_order_condition_ctp(OrderData* p_orderdata)
{
	// LOG(WARNING)<<"[verify_order_condition] calling verify order conditioin.................";
	std::time_t now_time = std::time(nullptr);
	int signal_delay = now_time - p_orderdata->order_insert_time;
	if(signal_delay > p_strategy_config->signal_delay){
		LOG(WARNING)<<"[verify_order_condition] Skip order with signal delay =>"<<signal_delay<<",delay config=>"<<p_strategy_config->signal_delay;
		return OrderVerify_unvalid;
	}
	std::vector<ptr_Position> v_pos = p_trader_handler->get_positions(p_orderdata->symbol);
	
	LOG(WARNING)<<"[verify_order_condition] return for get_position,size:"<<v_pos.size();

	int _total_pos_vol = 0;
	for(auto it=v_pos.begin(); it != v_pos.end(); ++it){
		LOG(WARNING)<<(*it)->InstrumentID<<","<<(*it)->TodayPosition<<","<<(*it)->OpenVolume<<","<<(*it)->CloseVolume<<","<<(*it)->PosiDirection<<std::endl;
		_total_pos_vol += (*it)->TodayPosition;
	}
	
	auto _tmp_daily_cache = this->m_daily_cache.find(p_orderdata->symbol);
	double _up_limit_price = 0.0;
	double _down_limit_price = 0.0;
	auto it = this->m_daily_cache.find(p_orderdata->symbol);
	int multiplier = get_instrument_multiplier(p_orderdata->symbol);
	if (multiplier==0){
		LOG(ERROR)<<"multiplier is 0 for instrument=>"<<p_orderdata->symbol;
	}
	if(it != this->m_daily_cache.end()){
		_up_limit_price = it->second->up_limit;
		_down_limit_price = it->second->down_limit;
	}
	
	if (_tmp_daily_cache != this->m_daily_cache.end()){
		_up_limit_price = _tmp_daily_cache->second->up_limit;
		_down_limit_price = _tmp_daily_cache->second->down_limit;
	}
	

	if(p_orderdata->status == LONG_SIGNAL){ // long signal
		LOG(WARNING)<<"[verify_order_condition] check long signal";

		for(auto it=v_pos.begin(); it!=v_pos.end();++it){
			ptr_Position p_curr_pos = *it;
			double vwap = p_curr_pos->OpenCost/(p_curr_pos->TodayPosition*multiplier);
			if(p_curr_pos->PosiDirection == THOST_FTDC_PD_Short){//long signal and short position, close the position for all vol by stop_profit
				LOG(WARNING)<<"[verify_order_condition] long signal, close short position";
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = floor(vwap - p_strategy_config->stop_profit);//买入平仓，下取整
				p_orderdata->volume = p_curr_pos->TodayPosition;
				p_orderdata->side = OrderSide_Buy;
				p_orderdata->position_effect = PositionEffect_Close;
				return OrderVerify_valid;
			}else if(p_curr_pos->PosiDirection == THOST_FTDC_PD_Long){//long signal and long position, open long untill vol_limit
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = _up_limit_price; //it is used as market data, with the high limit price
				//REMARK restrict vol limit for the whole account, not pos level
				// p_orderdata->volume = p_strategy_config->vol_limit - p_curr_pos->volume;
				p_orderdata->volume = p_strategy_config->vol_limit - _total_pos_vol; //FIXME this only need to support for multi-strategy case
				LOG(WARNING)<<"[verify_order_condition] long signal, open long position,order volume=>"<<p_orderdata->volume<<",vol_limit=>"<<p_strategy_config->vol_limit;
				if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
				
				p_orderdata->side = OrderSide_Buy;
				p_orderdata->position_effect = PositionEffect_Open;
				return OrderVerify_valid;
			}
		}
		if(v_pos.size()==0){//long signal and no positions,open long until vol_limit
			p_orderdata->order_type = OrderType_Limit;
			p_orderdata->volume = p_strategy_config->vol_limit;
			p_orderdata->price = _up_limit_price; //it is used as market data, with the high limit price
			LOG(WARNING)<<"[verify_order_condition] long signal, open long position, order volume=>"<<p_strategy_config->vol_limit;
			if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
			p_orderdata->side = OrderSide_Buy;
			p_orderdata->position_effect = PositionEffect_Open;
			return OrderVerify_valid;
		}
	}else if (p_orderdata->status==SHORT_SIGNAL){ //short signal
		LOG(WARNING)<<"[verify_order_condition] check short signal";
		for(auto it=v_pos.begin(); it!=v_pos.end();++it){
			ptr_Position p_curr_pos = *it;
			double vwap = p_curr_pos->OpenCost/(p_curr_pos->TodayPosition*multiplier);
			// std::cout<<"pos direction=>"<<p_curr_pos->PosiDirection<<std::endl;
			if(p_curr_pos->PosiDirection == THOST_FTDC_PD_Long){//short signal and long position, close the position for all vol by stop_profit
				LOG(WARNING)<<"[verify_order_condition] short signal, close long position, order volume=>"<<p_curr_pos->TodayPosition;
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = ceil(vwap + p_strategy_config->stop_profit);//卖出平仓，上取整
				p_orderdata->volume = p_curr_pos->TodayPosition;
				p_orderdata->side = OrderSide_Sell;
				p_orderdata->position_effect = PositionEffect_Close;
				return OrderVerify_valid;
			}else if(p_curr_pos->PosiDirection == THOST_FTDC_PD_Short){//short signal and short position, open long untill vol_limit
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = _down_limit_price;
				//REMARK restrict vol limit for the whole account, not pos level
				// p_orderdata->volume = p_strategy_config->vol_limit - p_curr_pos->volume;
				p_orderdata->volume = p_strategy_config->vol_limit - _total_pos_vol;//FIXME this only need to support for multi-strategy case
				LOG(WARNING)<<"[verify_order_condition] short signal, open short position,order volume=>"<<p_orderdata->volume;
				if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
				p_orderdata->side = OrderSide_Sell;
				p_orderdata->position_effect = PositionEffect_Open;
				return OrderVerify_valid;
			}
		}
		if(v_pos.size()==0){ //short signal and no positions, open short until vol_limit
			p_orderdata->order_type = OrderType_Limit;
			p_orderdata->price = _down_limit_price;
			p_orderdata->volume = p_strategy_config->vol_limit;
			LOG(WARNING)<<"[verify_order_condition] short signal, open short position, order volume=>"<<p_strategy_config->vol_limit;
			if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
			p_orderdata->side = OrderSide_Sell;
			p_orderdata->position_effect = PositionEffect_Open;
			return OrderVerify_valid;
		}		
	}else if (p_orderdata->status == STOP_SIGNAL){//check whether stop profit/loss,由于仿真没法下条件单，这相当于手工实现条件单，实际上CTP可以下条件单
		LOG(WARNING)<<"[verify_order_condition] stop profit and loss order, order volume=>"<<p_orderdata->volume;
		return OrderVerify_valid;
	}
	LOG(WARNING)<<"[verify_order_condition] return invalid order";
	return OrderVerify_unvalid;
}

/*
verify and update order dada for gm sim trade
LONG_SIGNAL:
	- short position: close the positions for all volume  with limit order by stop_profit
	- long position: open  long  positions with market order until vol limit
SHORT_SIGNAL:
	- long position: close the positions for all volume with limit order by stop profit
	- short position: open short positions with market order until vol limit 
STOP_SIGNAL:
	- stop profit and loss order
*/
/*
int QTStrategyBase::verify_order_condition(OrderData* p_orderdata)
{
	LOG(WARNING)<<"[verify_order_condition] calling verify order conditioin.................";
	std::time_t now_time = std::time(nullptr);
	int signal_delay = now_time - p_orderdata->order_insert_time;
	if(signal_delay > p_strategy_config->signal_delay){
		LOG(WARNING)<<"[verify_order_condition] Skip order with signal delay =>"<<signal_delay<<",delay config=>"<<p_strategy_config->signal_delay;
		return OrderVerify_unvalid;
	}
	std::vector<Position *> v_pos = simtrade_ptr->get_positions(p_orderdata->symbol);
	LOG(WARNING)<<"[verify_order_condition] return for get_position,size:"<<v_pos.size();
	int _total_pos_vol = 0;
	bool except = false; //REMARK gm callback issue remove this hack for system error, ugply but work for now
	for(auto it=v_pos.begin(); it!=v_pos.end();++it){
		Position* p_curr_pos = *it;
		// std::cout<<"??????????????cur pos in verify, vol=>"<<p_curr_pos->volume<<",side=>"<<p_curr_pos->side<<",vwap=>"<<p_curr_pos->vwap<< std::endl;
		if(p_curr_pos->side == OrderSide_Unknown) except = true;
		_total_pos_vol += p_curr_pos->volume;
	}
	if(except && _total_pos_vol >= p_strategy_config->vol_limit){
		LOG(WARNING)<<"[verify_order_condition] Except for position side and Exceed vol limit, return unvalid order";
		return OrderVerify_unvalid;
	}
	if(p_orderdata->status == LONG_SIGNAL){ // long signal
		LOG(WARNING)<<"[verify_order_condition] check long signal";
		for(auto it=v_pos.begin(); it!=v_pos.end();++it){
			Position* p_curr_pos = *it;
			// std::cout<<"?????????????cur pos vol=>"<<p_curr_pos->volume<<"cur pos side=>"<<p_curr_pos->side<<",vol=>"<<p_curr_pos->volume<<",vwap=>"<<p_curr_pos->vwap<<std::endl;
			if(p_curr_pos->side == OrderSide_Sell){//long signal and short position, close the position for all vol by stop_profit
				LOG(WARNING)<<"[verify_order_condition] long signal, close short position";
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = p_curr_pos->vwap - p_strategy_config->stop_profit;
				p_orderdata->volume = p_curr_pos->volume;
				p_orderdata->side = OrderSide_Buy;
				p_orderdata->position_effect = PositionEffect_Close;
				return OrderVerify_valid;
			}else if(p_curr_pos->side == OrderSide_Buy){//long signal and long position, open long untill vol_limit
				p_orderdata->order_type = OrderType_Market;
				//REMARK restrict vol limit for the whole account, not pos level
				// p_orderdata->volume = p_strategy_config->vol_limit - p_curr_pos->volume;
				p_orderdata->volume = p_strategy_config->vol_limit - _total_pos_vol;
				LOG(WARNING)<<"[verify_order_condition] long signal, open long position,order volume=>"<<p_orderdata->volume<<",vol_limit=>"<<p_strategy_config->vol_limit;
				if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
				
				p_orderdata->side = OrderSide_Buy;
				p_orderdata->position_effect = PositionEffect_Open;
				return OrderVerify_valid;
			}
		}
		if(v_pos.size()==0){//long signal and no positions,open long until vol_limit
			p_orderdata->order_type = OrderType_Market;
			p_orderdata->volume = p_strategy_config->vol_limit;
			LOG(WARNING)<<"[verify_order_condition] long signal, open long position, order volume=>"<<p_strategy_config->vol_limit;
			if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
			p_orderdata->side = OrderSide_Buy;
			p_orderdata->position_effect = PositionEffect_Open;
			return OrderVerify_valid;
		}
	}else if (p_orderdata->status==SHORT_SIGNAL){ //short signal
		LOG(WARNING)<<"[verify_order_condition] check short signal";
		for(auto it=v_pos.begin(); it!=v_pos.end();++it){
			Position* p_curr_pos = *it;
			if(p_curr_pos->side == OrderSide_Buy){//short signal and long position, close the position for all vol by stop_profit
				LOG(WARNING)<<"[verify_order_condition] short signal, close long position, order volume=>"<<p_curr_pos->volume;
				p_orderdata->order_type = OrderType_Limit;
				p_orderdata->price = p_curr_pos->vwap + p_strategy_config->stop_profit;
				p_orderdata->volume = p_curr_pos->volume;
				p_orderdata->side = OrderSide_Sell;
				p_orderdata->position_effect = PositionEffect_Close;
				return OrderVerify_valid;
			}else if(p_curr_pos->side == OrderSide_Sell){//short signal and short position, open long untill vol_limit
				p_orderdata->order_type = OrderType_Market;
				//REMARK restrict vol limit for the whole account, not pos level
				// p_orderdata->volume = p_strategy_config->vol_limit - p_curr_pos->volume;
				p_orderdata->volume = p_strategy_config->vol_limit - _total_pos_vol;
				LOG(WARNING)<<"[verify_order_condition] short signal, open short position,order volume=>"<<p_orderdata->volume;
				if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
				p_orderdata->side = OrderSide_Sell;
				p_orderdata->position_effect = PositionEffect_Open;
				return OrderVerify_valid;
			}
		}
		if(v_pos.size()==0){ //short signal and no positions, open short until vol_limit
			p_orderdata->order_type = OrderType_Market;
			p_orderdata->volume = p_strategy_config->vol_limit;
			LOG(WARNING)<<"[verify_order_condition] short signal, open short position, order volume=>"<<p_strategy_config->vol_limit;
			if(p_orderdata->volume <= 0) return OrderVerify_unvalid;
			
			p_orderdata->side = OrderSide_Sell;
			p_orderdata->position_effect = PositionEffect_Open;
			return OrderVerify_valid;
		}		
	}else if (p_orderdata->status == STOP_SIGNAL){//check whether stop profit/loss,由于仿真没法下条件单，这相当于手工实现条件单，实际上CTP可以下条件单
		LOG(WARNING)<<"[verify_order_condition] stop profit and loss order, order volume=>"<<p_orderdata->volume;
		return OrderVerify_valid;
	}
	LOG(WARNING)<<"[verify_order_condition] return invalid order";
	return OrderVerify_unvalid;
}
*/

void QTStrategyBase::process_order()
{
	LOG(WARNING)<<"[process_order] calling process_order:is active:"<<active_;
	bool _terminate = false;
	while(!_terminate) 
	{
		DataField data = this->p_order_queue->pop();
		if (data._data)
		{
			// LOG(WARNING)<<"[process_order] get order data, data field is:"<<data.data_type<<","<<data._data;
			switch (data.data_type)
			{
			case ORDERFIELDCTP://ctp trade
			{
				OrderData *p_orderdata = reinterpret_cast<OrderData *>(data._data);
				if(p_orderdata->status == TERMINATE_SIGNAL){
					LOG(WARNING)<<"[process_order] received terminate signal, cancel orders and close positions";
					int ret = this->cancel_all_orders();
					LOG(WARNING)<<"[process_order] received terminate signal, return from cancel all orders=>"<<ret;
					sleep(2);
					ret = this->close_all_orders();
					start_ = false; //TODO double check whether this is safe
					_terminate = true;
					LOG(WARNING)<<"[process_order] received terminate signal, return from close all orderrs=>"<<ret<<", start_=>"<<start_<<", _terminate=>"<<_terminate;
					break;
				}
				LOG(WARNING)<<"[process_order] ready place order :symbol:"<<p_orderdata->symbol<<",side:"<<p_orderdata->side<<",order status=>"<<p_orderdata->status<<",order vol=>"<<p_orderdata->volume;
				if(verify_order_condition_ctp(p_orderdata)==OrderVerify_valid){
					this->p_trader_handler->insert_order(p_orderdata);
				}else{
					LOG(WARNING)<<"[process_order] order invalid, ignore signal";
				}
				break;
			}
			case ORDERFIELDSIM://gm simulation
			{
				// OrderData *p_orderdata = reinterpret_cast<OrderData *>(data._data);
				// if(p_orderdata->status == TERMINATE_SIGNAL){
				// 	_terminate = true;
				// 	break;
				// }
				// LOG(WARNING)<<"[process_order] ready place order :symbol:"<<p_orderdata->symbol<<",side:"<<p_orderdata->side<<",order status=>"<<p_orderdata->status<<",order vol=>"<<p_orderdata->volume;
				// if(verify_order_condition(p_orderdata)==OrderVerify_valid){
				// 	this->simtrade_ptr->insert_order(p_orderdata);
				// }
				// break;
			}
			default:
				break;
			}//end of switch
		}//end of if 
		if(_terminate) 
		{
			LOG(WARNING)<<"[process_order] break process order";
			break;
		}
	}//end of while
}//end of process_order

void QTStrategyBase::cache_main_instruments(std::vector<std::string> _v_instrument_id)
{
	LOG(WARNING)<<"[cache_main_instruments]......";
	std::vector<CThostFtdcInstrumentField*> ret_instruments = get_instruments(_v_instrument_id);
	std::vector<CThostFtdcDepthMarketDataField*> ret_depth_market_data = get_market_datas(_v_instrument_id);
	std::unordered_map<std::string, double> m_ins2openinterest;
	//cache {instrument_id:open_interest}
	for(auto it=ret_depth_market_data.begin(); it!=ret_depth_market_data.end();++it)
	{
		std::string _ins_id = (*it)->InstrumentID;
		CThostFtdcDepthMarketDataField* p_tmp = reinterpret_cast<CThostFtdcDepthMarketDataField*>(*it);
		m_ins2openinterest[_ins_id] = p_tmp->OpenInterest;
	}
	// cal main future contracts group by productid, sort by open interest
	for (auto it=ret_instruments.begin(); it!=ret_instruments.end(); ++it)
	{
		CThostFtdcInstrumentField* p_tmp = reinterpret_cast<CThostFtdcInstrumentField*>(*it);
		if(p_tmp->ProductClass=='1'){//single future 
			if(m_product_exchangeid.find(p_tmp->InstrumentID) == m_product_exchangeid.end()){ //if product to exchange mapping not exist, add the pair
				m_product_exchangeid.insert(std::pair<std::string, std::string>(p_tmp->InstrumentID, p_tmp->ExchangeID));
			}
			if(m_main_futures.find(p_tmp->ProductID)==m_main_futures.end()){//"key not exist"
				m_main_futures.insert(std::pair<std::string, std::string>(p_tmp->ProductID, p_tmp->InstrumentID));
			}else{ //"key exist"
				std::string _prev_ins_id = m_main_futures[p_tmp->ProductID];
				if(m_ins2openinterest.find(_prev_ins_id)==m_ins2openinterest.end()){ 
					m_main_futures.insert(std::pair<std::string, std::string>(p_tmp->ProductID, p_tmp->InstrumentID));
				}else if(m_ins2openinterest.find(p_tmp->InstrumentID)!=m_ins2openinterest.end()){
					if(m_ins2openinterest[p_tmp->InstrumentID] > m_ins2openinterest[_prev_ins_id]){
						m_main_futures[p_tmp->ProductID] = p_tmp->InstrumentID;
					}
				}
			}
		}
	}
	//get main future ids by cal results
	for(const auto& n:m_main_futures)
	{
		LOG(WARNING)<<"[cache_main_instruments] push  main contract future id in cache main contract->"<<n.first<<","<<n.second;
		v_main_contract_ids.push_back(n.second);
	}

	//get target options id 
	std::unordered_map<std::string, std::vector<std::pair<std::string, double>>> m_option_val;
	//{main_contract:[option_id, open_interest]}
	for (auto it=ret_instruments.begin(); it!=ret_instruments.end(); ++it)
	{
		CThostFtdcInstrumentField* p_tmp = reinterpret_cast<CThostFtdcInstrumentField*>(*it);
		if(p_tmp->ProductClass=='2'){//single option
			std::string _underlying_id = p_tmp->UnderlyingInstrID;
			auto it = find(v_main_contract_ids.begin(), v_main_contract_ids.end(), _underlying_id);
			if(it != v_main_contract_ids.end()){ //underlying instrument is in main contract
				if(m_ins2openinterest.find(p_tmp->InstrumentID)!=m_ins2openinterest.end()){
					m_option_val[_underlying_id].push_back(std::pair<std::string, double>(p_tmp->InstrumentID, m_ins2openinterest[p_tmp->InstrumentID]));
				}
			}
		}
	}

	//calculate target option id lst
	for(auto& t:m_option_val){
		LOG(WARNING)<<t.first<<"[cache_main_instruments] option size:"<<t.second.size();
		sort(t.second.begin(), t.second.end(), [](const std::pair<std::string, double>& x, const std::pair<std::string, double>& y) -> bool {return x.second<y.second;});
		const auto it = t.second.rbegin();
		int cnt = 0;
		while(cnt<option_size){
			if(it+cnt != t.second.rend())
			{
				v_option_ids.push_back((*(it+cnt)).first);
				cnt ++;
			}
		}
		LOG(WARNING)<<"[cache_main_instruments] complete push option ids";
	}
	for(auto it=v_option_ids.begin(); it!=v_option_ids.end();++it)
	{
		LOG(WARNING)<<"[cache_main_instruments] v_option_id=>"<<*it;
	}
	LOG(WARNING)<<"[cache_main_instruments] start cache target instruments......";

	for(auto it=ret_instruments.begin(); it!=ret_instruments.end();++it){
		CThostFtdcInstrumentField* p_tmp = new CThostFtdcInstrumentField();
		p_tmp = reinterpret_cast<CThostFtdcInstrumentField*>(*it);
		std::string _instrument_id = p_tmp->InstrumentID;


		auto f_it = find(v_main_contract_ids.begin(), v_main_contract_ids.end(), _instrument_id);
	
		if(f_it!=v_main_contract_ids.end()){
			// m_instrument_fields.insert(std::pair<std::string, CThostFtdcInstrumentField*>(_instrument_id,p_tmp));
			m_vol_multiple.insert(std::pair<std::string, int>(_instrument_id, p_tmp->VolumeMultiple));
		}

		auto o_it = find(v_option_ids.begin(), v_option_ids.end(), _instrument_id);
		if(o_it!=v_option_ids.end()){
			// m_instrument_fields.insert(std::pair<std::string, CThostFtdcInstrumentField*>(_instrument_id,p_tmp));
			m_vol_multiple.insert(std::pair<std::string, int>(_instrument_id, p_tmp->VolumeMultiple));
		}
	}
	

	for(auto it =ret_instruments.begin(); it!=ret_instruments.end();++it)
	{
		delete *it;
	}

	for(auto it=ret_depth_market_data.begin(); it!=ret_depth_market_data.end(); ++it)
	{
		delete *it;
	}


}


int QTStrategyBase::cancel_all_orders(){
	if(this->mode== 1){
		// return this->simtrade_ptr->cancel_all_orders();
	}else if(this->mode == 2){
		return p_trader_handler->cancel_all_orders();
	}else{
		LOG(WARNING)<<"[cancel_all_orders] invalid mode";
	}
	return 0;
}

//close all positions
int QTStrategyBase::close_all_orders(){
	if(this->mode== 1){
		// this->simtrade_ptr->order_close_all();
	}else if(this->mode == 2){
		return p_trader_handler->close_all_positions();
	}else{
		LOG(WARNING)<<"[close_all_orders] invalid mode";
	}
	return 0;
}


/*
监控账户的风险度和订�?/持仓状况
if 到达收盘时间�?
    - 撤单（未结委托）
    - 平仓（不暴露隔夜风险�?
else�?
    判断是否有超时平仓的需求，拿到现订单，看update 时间和现在间隔，超过一定时间则平仓
    sleep(xx)
else:
    判断是否止盈止损，拿到持仓，如果有触发止盈止损的订单，则调用止盈止损
else:
    风险度，保证金等其他监控指标
*/
int QTStrategyBase::risk_monitor(RiskInputData* p_risk_input, StrategyConfig* p_strategy_conf){

    std::time_t now_time = std::time(nullptr);
	// std::cout<<"check risk monitor=>"<<now_time<<std::endl;
    tm *ltm = localtime(&now_time);
	// LOG(WARNING)<<"[risk_monitor] Start risk monitor, min=>"<<ltm->tm_min<<",sec=>"<<ltm->tm_sec;
    if(ltm->tm_hour == 14 && ltm->tm_min == 58){//for backup, if not close for terminate signal
        LOG(WARNING)<<"[risk_monitor] market closed, close all positions";
		this->cancel_all_orders();
		//FIXME cancel没有做订单锁定处�?
		sleep(2);
		this->close_all_orders();
		start_ = false; //TODO double check whether this is safe
		LOG(WARNING)<<"[risk_monitor] return for cancel orders and close positions";
		return 0; //to break the risk thread
	//TODO remove this ,double check, only cancel for the delayed order
    // }else if(ltm->tm_sec < 5){ //FIXME hardcode this delay buffer, should changed to be adapted,整分钟时候未必会调用,所以尝试设置多两秒buffer
		// int ret = this->cancel_all_orders();
        // LOG(WARNING)<<"[risk_monitor] cancel all orders for mintues with return =>"<<ret;
    }else{// 定期检查未成订单以及持仓，订单超时
		//order data params which derive from conf and mkt
        double _last_price = p_risk_input->last_price;
        std::string _exchangeid = p_risk_input->exchangeid;
		double stop_profit_bc = p_strategy_conf->stop_profit;
		double stop_loss_bc = p_strategy_conf->stop_loss;
		if(this->mode == 1){ // handle gm simtrade case,cancel stale unfinished order, stop profit and loss, 基本不采用，TODO,remove�?
        	// gmtrade::DataArray<Order>* ret_order = simtrade_ptr->get_unfinished_orders(simtrade_account_id.c_str());
        	// if(ret_order->status()==0){
        	//     for(int i=0; i<ret_order->count();++i){
        	//         Order &_tmp = ret_order->at(i);
        	//         std::time_t order_delay = now_time - _tmp.updated_at;
        	//         std::cout<<"order delay=>"<<order_delay<<",order update time=>"<<_tmp.updated_at<<",now time=>"<<now_time<<std::endl;
        	//         int ret;
        	//         if(order_delay > p_strategy_conf->cancel_order_delay){
        	//             ret = cancel_all_orders();//FIXME should call order_cancle, but order_cancel has some issue
        	//             // ret = order_cancel(_tmp.order_id, account_id.c_str());
        	//             sleep(1);
        	//             LOG(WARNING)<<"[risk_monitor] cancel order for order delay=>"<<order_delay<<"cancel order return=>"<<ret<<"cancel order id=>"<<_tmp.order_id<<"order vol=>"<<_tmp.volume;
        	//         }
        	//     }//end of for order loop
        	// ret_order->release();
			// }

			// std::vector<ptr_position> ret_all_pos = this->simtrade_ptr->get_positions();
			// for(auto it = ret_all_pos.begin(); it != ret_all_pos.end(); ++it){
			//     ptr_position  _cur_pos = *it;
			//     bool stop_profit = (_cur_pos->side==PositionSide_Long && _last_price-_cur_pos->vwap >stop_profit_bc) || (_cur_pos->side==PositionSide_Short && _last_price-_cur_pos->vwap<-stop_profit_bc);
			//     bool stop_loss = (_cur_pos->side==PositionSide_Long && _last_price-_cur_pos->vwap <-stop_loss_bc) || (_cur_pos->side==PositionSide_Short && _last_price-_cur_pos->vwap>stop_loss_bc);
			//     // if(stop_profit || stop_loss)
			//     LOG(WARNING)<<"[risk_monitor] cur pos side=>"<<_cur_pos->side<<",cur pos vwap=>"<<_cur_pos->vwap<<",cur pos vol=>"<<_cur_pos->volume<<",last price=>"<<_last_price;
			//     OrderData* p_order = new OrderData();
			//     p_order->order_type = OrderType_Limit;
			//     p_order->volume = _cur_pos->volume;
			//     p_order->position_effect = PositionEffect_Close;
			//     p_order->symbol = _exchangeid + "."+p_risk_input->symbol;
			// 	p_order->order_insert_time = now_time;
			// 	p_order->status = STOP_SIGNAL;
			//     if(_cur_pos->side == PositionSide_Long && stop_profit){//long position and stop profit
			//         p_order->side = OrderSide_Sell;
			//         p_order->price = _last_price;
			//     }else if(_cur_pos->side == PositionSide_Long && stop_loss){
			//         p_order->side = OrderSide_Sell;
			//         p_order->price = _last_price;
			//     }else if(_cur_pos->side == PositionSide_Short && stop_profit){
			//         p_order->side = OrderSide_Buy;
			//         p_order->price = _last_price;
			//     }else if(_cur_pos->side == PositionSide_Short && stop_loss){
			//         p_order->side = OrderSide_Buy;
			//         p_order->price = _last_price;
			//     }else{
			//         // idle
			//     }
			//     if((stop_profit || stop_loss) && (p_order->volume>0 && p_order->price>1.0)){
			//         LOG(WARNING) <<"[risk_monitor] insert order for stop_profit or loss,stop_profit=>"<<stop_profit<<",stop loss=>"<<stop_loss;
			// 		place_order(p_order);
			//         // LOG(WARNING)<<"[risk_monitor] order return status=>"<<p_order_ret->status<<",order rej reason=>"<<p_order_ret->ord_rej_reason<<",rej details=>"<<p_order_ret->ord_rej_reason_detail;
			//     }
			// }//end of position loop
        }else if(mode == 2){ //ctp cancel stale orders 
			//cancel stale orders
			std::vector<ptr_OrderField> v_ret_orders = p_trader_handler->get_all_orders();
			for(auto it = v_ret_orders.begin(); it != v_ret_orders.end(); ++it){
    			ptr_OrderField p_cur_order = *it;
    			std::time_t order_delay = now_time - p_cur_order->InsertTime;
    			// LOG(WARNING)<<"[risk monitor] order delay=>"<<order_delay<<",order update time=>"<<p_cur_order->InsertTime<<",now time=>"<<now_time<<"order delay conf=>"<<p_strategy_conf->cancel_order_delay;
    			int ret;
    			if(order_delay > p_strategy_conf->cancel_order_delay && p_cur_order->OrderStatus != THOST_FTDC_OST_AllTraded && p_cur_order->OrderStatus != THOST_FTDC_OST_Canceled){
					LOG(WARNING)<<"[risk_monitor] cancel stale order in order table,order id"<<p_cur_order->order_id<<", order vol=>"<<p_cur_order->VolumeTotalOriginal<<", order ref=>"<<p_cur_order->p_orderid_ref->OrderRef<<", order sys ref=>"<<p_cur_order->p_orderid_ref->OrderSysID;
					ret = p_trader_handler->cancel_order(p_cur_order->p_orderid_ref);
					//FIXME double check, 撤单没有做订单锁�?
    		    	sleep(1);
    		    	LOG(WARNING)<<"[risk_monitor] ret from cancel order for order delay=>"<<order_delay<<", cancel order return=>"<<ret<<", cancel order id=>"<<p_cur_order->order_id<<", order vol=>"<<p_cur_order->VolumeTotalOriginal<<", order ref=>"<<p_cur_order->p_orderid_ref->OrderRef<<", order sys ref=>"<<p_cur_order->p_orderid_ref->OrderSysID;
    			}
			}//end of for order loop

			//stop profit and loss
			// std::cout<<"before check pos size for risk monitor"<<std::endl;
			std::vector<ptr_Position> v_ret_pos = p_trader_handler->get_positions(p_risk_input->symbol);
			// std::cout<<"check pos size for risk monitor=>"<<v_ret_pos.size()<<std::endl;
			for(auto it = v_ret_pos.begin(); it != v_ret_pos.end(); ++it){ 
				
			    ptr_Position  _cur_pos = *it;
				// std::cout<<"check risk monitor for pos=>"<<_cur_pos->InstrumentID<<"cost=>"<<_cur_pos->OpenCost<<std::endl;
				int multiplier = get_instrument_multiplier(_cur_pos->InstrumentID);
				double vwap = _cur_pos->OpenCost/(_cur_pos->TodayPosition*multiplier);
			    bool stop_profit = (_cur_pos->PosiDirection ==THOST_FTDC_PD_Long && _last_price-vwap>stop_profit_bc) || (_cur_pos->PosiDirection==THOST_FTDC_PD_Short && _last_price-vwap<-stop_profit_bc);
			    bool stop_loss = (_cur_pos->PosiDirection==THOST_FTDC_PD_Long && _last_price-vwap <-stop_loss_bc) || (_cur_pos->PosiDirection==THOST_FTDC_PD_Short && _last_price-vwap>stop_loss_bc);
			    // if(stop_profit || stop_loss)
			    // LOG(WARNING)<<"[risk_monitor] cur pos side=>"<<_cur_pos->PosiDirection<<",cur pos vwap=>"<<_cur_pos->OpenCost<<",cur pos vol=>"<<_cur_pos->TodayPosition<<",last price=>"<<_last_price;
			    OrderData* p_order = new OrderData();
			    p_order->order_type = OrderType_Limit;
			    p_order->volume = _cur_pos->TodayPosition;
			    p_order->position_effect = PositionEffect_Close;
			    // p_order->symbol = _exchangeid + "."+p_risk_input->symbol;
				p_order->symbol = p_risk_input->symbol;
				p_order->exchangeid = _exchangeid;
				p_order->order_insert_time = now_time;
				p_order->status = STOP_SIGNAL;
			    if(_cur_pos->PosiDirection == THOST_FTDC_PD_Long && stop_profit){//long position and stop profit
			        p_order->side = OrderSide_Sell;
			        p_order->price = _last_price;
			    }else if(_cur_pos->PosiDirection == THOST_FTDC_PD_Long && stop_loss){
			        p_order->side = OrderSide_Sell;
			        p_order->price = _last_price;
			    }else if(_cur_pos->PosiDirection == THOST_FTDC_PD_Short && stop_profit){
			        p_order->side = OrderSide_Buy;
			        p_order->price = _last_price;
			    }else if(_cur_pos->PosiDirection == THOST_FTDC_PD_Short && stop_loss){
			        p_order->side = OrderSide_Buy;
			        p_order->price = _last_price;
			    }else{
			        // idle
			    }
			    if((stop_profit || stop_loss) && (p_order->volume>0 && p_order->price>1.0)){
			        LOG(WARNING) <<"[risk_monitor] insert order for stop_profit or loss,stop_profit=>"<<stop_profit<<",stop loss=>"<<stop_loss;
					place_order(p_order);
			        // LOG(WARNING)<<"[risk_monitor] order return status=>"<<p_order_ret->status<<",order rej reason=>"<<p_order_ret->ord_rej_reason<<",rej details=>"<<p_order_ret->ord_rej_reason_detail;
			    }
			}//end of position loop
		}
    }
    return 0;
}