#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <queue>
#include <atlstr.h>

#include "CDataBase.h"
#include "protocol.h"
#include "CAStar.h"

#pragma comment (lib, "WS2_32.lib")
#pragma comment (lib, "mswsock.lib")
#pragma comment (lib, "lua53.lib")

extern "C"
{
#include "lua.h"
#include "luaconf.h"
#include "lauxlib.h"
#include "lualib.h"
}

using namespace std;
using namespace chrono;


enum ENUMOP { OP_RECV , OP_SEND, OP_ACCEPT, OP_RANDOM_MOVE , OP_PLAYER_MOVE, OP_PLAYER_HP_HEAL, OP_NPC_RESPAWN, OP_NPC_ATTACK};

enum C_STATUS {ST_FREE, ST_ALLOCATED, ST_ACTIVE, ST_SLEEP};


struct event_type
{
	int obj_id;
	ENUMOP event_id; //힐링, 이동 ...
	high_resolution_clock::time_point wakeup_time;
	int target_id;

	constexpr bool operator < (const event_type& left) const
	{
		return (wakeup_time > left.wakeup_time);
	}
};
priority_queue<event_type> timer_queue;
mutex timer_lock;

//확장 overlapped 구조체
struct EXOVER
{
	WSAOVERLAPPED	over;
	ENUMOP			op;						//send, recv, accpet 중 무엇인지 
	char			io_buf[MAX_BUF_SIZE];	//버퍼의 위치 관리
	
	union {
		WSABUF			wsabuf;					//포인터 넣을 바에야 차라리 버퍼 자체를 넣자. 한군데 같이 두면 확장구조체를 재사용하면 전체가 재사용 된다.
		SOCKET			c_socket;
		int				p_id; //움직인 player의 id
	};
};

//클라이언트 정보 저장 구조체
struct CLIENT
{
	mutex	m_cLock;
	SOCKET	m_socket;			//lock으로 보호
	int		m_id;				//lock으로 보호
	EXOVER	m_recv_over;
	int		m_prev_size; 
	char	m_packet_buf[MAX_PACKET_SIZE];		//조각난 거 받아두기 위한 버퍼
	int		m_db;

	atomic<C_STATUS> m_status;

	//게임 콘텐츠 
	short x, y;
	char m_name[MAX_ID_LEN + 1];			//lock으로 보호
	short level;
	int exp;
	short hp;

	//npc의 타입
	bool npcCharacterType; //0-peace / 1-war
	bool npcMoveType; //0-고정 / 1-로밍
	
	int** mapData;
	CAStar pathfind;
	CLIENT* target = nullptr;

	unsigned  m_move_time;
	unsigned  m_attack_time;

	//high_resolution_clock::time_point m_last_move_time;

	unordered_set<int> view_list; //순서가 상관없을 땐 unordered 쓰는게 더 속도가 빠르다 

	lua_State* L;
	mutex lua_l;

	int moveCount = 0;
};

struct DATABASE
{
	int id;
	char Name[20];
	int x, y;
	short level;
	int exp;
	short HP;
};

MAP g_Map[WORLD_HEIGHT][WORLD_WIDTH];

CLIENT g_clients[NPC_ID_START + NUM_NPC];		//클라이언트 동접만큼 저장하는 컨테이너 필요

list<DATABASE> g_dbData;

HANDLE g_iocp;					//iocp 핸들
SOCKET listenSocket;			//서버 전체에 하나. 한번 정해지고 안바뀌니 데이터레이스 아님. 

int g_totalUserCount = 5;

void LoadData() {
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	SQLWCHAR szUser_Name[NAME_LEN];
	SQLINTEGER dUser_id, dUser_x, dUser_y, dUser_LEVEL, dUser_EXP, dUser_HP;

	setlocale(LC_ALL, "korean");

	SQLLEN cbName = 0, cbID = 0, cbX, cbY = 0, cbHP, cbEXP, cbLEVEL;

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	// Set the ODBC version environment attribute  
	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"game_db_odbc", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

					printf("ODBC Connect OK \n");

					//SELECT 다음에 오는건 항목들, FROM 다음에 오는건 테이블 이름, ORDER BY 다음에 오는건 sorting 할 것. 지금은 user_name으로 정렬.
					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)L"EXEC select_allUser 0", SQL_NTS);
					//C:\Program Files\Microsoft SQL Server\MSSQL15.MSSQLSERVER\MSSQL\DATA
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {

						printf("Select OK \n");

						// Bind columns 1, 2, and 3  
						retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &dUser_id, 100, &cbID);
						retcode = SQLBindCol(hstmt, 2, SQL_C_WCHAR, szUser_Name, NAME_LEN, &cbName);
						retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &dUser_EXP, 100, &cbEXP);
						retcode = SQLBindCol(hstmt, 4, SQL_C_LONG, &dUser_HP, 100, &cbHP);
						retcode = SQLBindCol(hstmt, 5, SQL_C_LONG, &dUser_LEVEL, 100, &cbLEVEL);
						retcode = SQLBindCol(hstmt, 6, SQL_C_LONG, &dUser_x, 100, &cbX);
						retcode = SQLBindCol(hstmt, 7, SQL_C_LONG, &dUser_y, 100, &cbY);

						// Fetch and print each row of data. On an error, display a message and exit.  
						for (int i = 0; ; i++) {
							retcode = SQLFetch(hstmt);
							if (retcode == SQL_ERROR || retcode == SQL_SUCCESS_WITH_INFO)
							{
								show_error();
								HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);
							}
							if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO)
							{
								wprintf(L"ID :%d  NICK : %s \t(%d,%d)\t LEV : %d\t EXP : %d\t HP : %d\n", dUser_id, szUser_Name, dUser_x, dUser_y, dUser_LEVEL, dUser_EXP, dUser_HP);
								char *temp;
								int strSize = WideCharToMultiByte(CP_ACP, 0, szUser_Name, -1, NULL, 0, NULL, NULL);
								temp = new char[11];
								WideCharToMultiByte(CP_ACP, 0, szUser_Name, -1, temp, strSize, 0, 0);
								//if (isdigit(temp[strlen(temp) - 1]) == 0) 
								{
									//cout << "문자열공백제거\n";
									temp[strlen(temp) - 4] = '\0';
								}

								DATABASE newDB;
								newDB.id = dUser_id;
								newDB.x = dUser_x;
								newDB.y = dUser_y;
								newDB.level = dUser_LEVEL;
								newDB.HP = dUser_HP;
								newDB.exp = dUser_EXP;

								memcpy_s(newDB.Name, 10, temp ,10);
								cout << "'"<<newDB.Name<<"'"<< endl;
								g_dbData.push_back(newDB);
							}
							else
								break;
						}
					}

					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt);
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}
				else
					HandleDiagnosticRecord(hdbc, SQL_HANDLE_DBC, retcode);

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);

			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}

	wprintf(L"database end \n");
}

void UpdateData(int keyid, int x, int y, int level, int exp, int hp)
{
	SQLHENV henv;		// 데이터베이스에 연결할때 사옹하는 핸들
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0; // sql명령어를 전달하는 핸들
	SQLRETURN retcode;  // sql명령어를 날릴때 성공유무를 리턴해줌
	SQLWCHAR query[1024];
	wsprintf(query, L"UPDATE Table_3 SET user_x = %d, user_y = %d, user_EXP = %d, user_HP = %d, user_LEVEL = %d WHERE user_id = %d", x, y, exp, hp, level, keyid);

	for (auto &db : g_dbData)
	{
		if (db.id == keyid)
		{
			db.x = x;
			db.y = y;
			db.level = level;
			db.exp = exp;
			db.HP = hp;
		}
	}


	setlocale(LC_ALL, "korean"); // 오류코드 한글로 변환
	//std::wcout.imbue(std::locale("korean"));

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0); // ODBC로 연결

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0); // 5초간 연결 5초넘어가면 타임아웃

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"game_db_odbc", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);
				//retcode = SQLConnect(hdbc, (SQLWCHAR*)L"jys_gameserver", SQL_NTS, (SQLWCHAR*)NULL, SQL_NTS, NULL, SQL_NTS);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt); // SQL명령어 전달할 한들

					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)query, SQL_NTS); // 쿼리문
					//retcode = SQLExecDirect(hstmt, (SQLWCHAR *)L"EXEC select_highlevel 90", SQL_NTS); // 90레벨 이상만 가져오기

					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						printf( "DataBase update success \n user_x = %d, user_y = %d, user_EXP = %d, user_HP = %d, user_LEVEL = %d WHERE user_id = %d", 
							x, y, exp, hp, level, keyid);
					}
					

					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt); // 핸들캔슬
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}
}

wchar_t* ConverCtoWC(char* str)
{
	//wchar_t형 변수 선언
	wchar_t* pStr;
	//멀티 바이트 크기 계산 길이 반환
	int strSize = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, NULL);

	//wchar_t 메모리 할당
	pStr = new WCHAR[strSize];
	//형 변환
	MultiByteToWideChar(CP_ACP, 0, str, strlen(str) + 1, pStr, strSize);

	return pStr;
}

void InsertData(int keyid, char* name, int x, int y, int level, int exp, int hp)
{
	SQLHENV henv;		// 데이터베이스에 연결할때 사옹하는 핸들
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0; // sql명령어를 전달하는 핸들
	SQLRETURN retcode;  // sql명령어를 날릴때 성공유무를 리턴해줌
	SQLWCHAR query[1024];

	CString str = name;

	wsprintf(query, L"INSERT INTO Table_3 (user_name, user_x, user_y, user_EXP, user_HP, user_LEVEL, user_id) VALUES (\'%s\', %d, %d, %d, %d, %d, %d)",
		str, x, y, exp, hp, level, keyid);




	setlocale(LC_ALL, "korean"); // 오류코드 한글로 변환
	//std::wcout.imbue(std::locale("korean"));

	// Allocate environment handle  
	retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

	if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
		retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0); // ODBC로 연결

		// Allocate connection handle  
		if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
			retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

			// Set login timeout to 5 seconds  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0); // 5초간 연결 5초넘어가면 타임아웃

				// Connect to data source  
				retcode = SQLConnect(hdbc, (SQLWCHAR*)L"game_db_odbc", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);
				//retcode = SQLConnect(hdbc, (SQLWCHAR*)L"jys_gameserver", SQL_NTS, (SQLWCHAR*)NULL, SQL_NTS, NULL, SQL_NTS);

				// Allocate statement handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt); // SQL명령어 전달할 한들

					retcode = SQLExecDirect(hstmt, (SQLWCHAR *)query, SQL_NTS); // 쿼리문
					//retcode = SQLExecDirect(hstmt, (SQLWCHAR *)L"EXEC select_highlevel 90", SQL_NTS); // 90레벨 이상만 가져오기

					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						printf("DataBase insert success \n user_x = %d, user_y = %d, user_EXP = %d, user_HP = %d, user_LEVEL = %d WHERE user_id = %d",
							x, y, exp, hp, level, keyid);

						DATABASE db;
						db.x = x;
						db.y = y;
						db.level = level;
						db.exp = exp;
						db.HP = hp;

						g_dbData.push_back(db);
					}


					// Process data  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLCancel(hstmt); // 핸들캔슬
						SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
					}

					SQLDisconnect(hdbc);
				}

				SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
			}
		}
		SQLFreeHandle(SQL_HANDLE_ENV, henv);
	}

}

void add_timer(int obj_id, ENUMOP op_type, int duration)
{
	timer_lock.lock();
	event_type ev{ obj_id, op_type,high_resolution_clock::now() + milliseconds(duration), 0 };
	timer_queue.emplace(ev);
	timer_lock.unlock();
}

//lock으로 보호받고있는 함수
void send_packet(int user_id, void *p)
{
	char* buf = reinterpret_cast<char*>(p);

	CLIENT &user = g_clients[user_id];

	//WSASend의 두번째 인자의 over는 recv용이라 쓰면 안된다. 새로 만들어야 한다.
	EXOVER *exover = new EXOVER;
	exover->op = OP_SEND;
	ZeroMemory(&exover->over, sizeof(exover->over));
	exover->wsabuf.buf = exover->io_buf;
	exover->wsabuf.len = buf[0];
	memcpy(exover->io_buf, buf, buf[0]);

	WSASend(user.m_socket, &exover->wsabuf, 1, NULL, 0, &exover->over, NULL);
}
void send_login_ok_packet(int user_id)
{
	sc_packet_login_ok p;
	p.exp = 0;
	p.hp = 0;
	p.id = user_id;
	p.level = 0;
	p.size = sizeof(p);
	p.type = S2C_LOGIN_OK;
	p.x = g_clients[user_id].x;
	p.y = g_clients[user_id].y;

	send_packet(user_id, &p); //&p로 주지 않으면 복사되어서 날라가니까 성능에 안좋다. 


	sc_packet_stat_change psc;
	psc.size = sizeof(psc);
	psc.type = S2C_STAT_CHANGE;
	psc.id = user_id;
	psc.exp = g_clients[user_id].exp;
	psc.level = g_clients[user_id].level;
	psc.hp = g_clients[user_id].hp;
	send_packet(user_id, &psc);
}

void send_login_fail_packet()
{

}



//아이디에게, 누가 이동했는지 알려줘라
void send_move_packet(int user_id, int mover)
{
	sc_packet_move p;
	p.id = mover;
	p.size = sizeof(p);
	p.type = S2C_MOVE;
	p.x = g_clients[mover].x;
	p.y = g_clients[mover].y;
	p.move_time = g_clients[mover].m_move_time;

	send_packet(user_id, &p); //&p로 주지 않으면 복사되어서 날라가니까 성능에 안좋다. 
}

void send_chat_packet(int user_id, int chatter, char message[], int chatType)
{
	sc_packet_chat p;
	p.id = chatter;
	p.size = sizeof(p);
	p.type = S2C_CHAT;
	p.chatType = chatType;
	strcpy_s(p.mess, message);
	send_packet(user_id, &p);
}

void send_enter_packet(int user_id, int o_id)
{
	sc_packet_enter p;
	p.id = o_id;
	p.size = sizeof(p);
	p.type = S2C_ENTER;
	p.x = g_clients[o_id].x;
	p.y = g_clients[o_id].y;
	p.npcCharacterType = g_clients[o_id].npcCharacterType;
	p.npcMoveType = g_clients[o_id].npcMoveType;
	strcpy_s(p.name, g_clients[o_id].m_name);
	p.o_type = O_PLAYER;
	

	g_clients[user_id].m_cLock.lock();
	g_clients[user_id].view_list.insert(o_id);
	g_clients[user_id].m_cLock.unlock();

	send_packet(user_id, &p); //&p로 주지 않으면 복사되어서 날라가니까 성능에 안좋다. 


	sc_packet_stat_change psc;
	psc.size = sizeof(psc);
	psc.type = S2C_STAT_CHANGE;
	psc.id = o_id;
	psc.exp = g_clients[o_id].exp;
	psc.level = g_clients[o_id].level;
	psc.hp= g_clients[o_id].hp;
	send_packet(user_id, &psc);
}

//user_id에게 o_id의 상태가 변했음을 알려줌
void send_stat_change_packet(int user_id, int o_id)
{
	sc_packet_stat_change psc;
	psc.size = sizeof(psc);
	psc.type = S2C_STAT_CHANGE;
	psc.id = o_id;
	psc.exp = g_clients[o_id].exp;
	psc.level = g_clients[o_id].level;
	psc.hp = g_clients[o_id].hp;
	send_packet(user_id, &psc);
}

//void send_near_packet(int client, int new_id)
//{
//	// new_id가 들어왔고 이 new id의 정보를 client에게 보내는 것
//	sc_packet_near packet;
//	packet.id = new_id;
//	packet.size = sizeof(packet);
//	packet.type = S2C_NEAR_PLAYER;
//	packet.x = g_clients[new_id].x;
//	packet.y = g_clients[new_id].y;
//	send_packet(client, &packet);
//}
void send_leave_packet(int user_id, int o_id)
{
	sc_packet_move p;
	p.id = o_id;
	p.size = sizeof(p);
	p.type = S2C_LEAVE;

	g_clients[user_id].m_cLock.lock();
	g_clients[user_id].view_list.erase(o_id);
	g_clients[user_id].m_cLock.unlock();

	send_packet(user_id, &p); //&p로 주지 않으면 복사되어서 날라가니까 성능에 안좋다. 
}

//a 와 b가 서로 시야에 있나? 
bool is_near(int a, int b)
{
	if (abs(g_clients[a].x - g_clients[b].x) > VIEW_RADIUS) return false;
	if (abs(g_clients[a].y - g_clients[b].y) > VIEW_RADIUS) return false;

	return true;
}

bool is_player(int id)
{
	return id < NPC_ID_START;
}

//SLEEP에서 ACTIVE로 바꾼것에 성공한 경우만 add_timer. 한번만 하지 않으면 2배 속도로 이동
void activate_npc(int id)
{
	//g_clients[id].m_status = ST_ACTIVE;
	C_STATUS old_status = ST_SLEEP;
	if (true == atomic_compare_exchange_strong(&g_clients[id].m_status, &old_status, ST_ACTIVE)) 
		add_timer(id, OP_RANDOM_MOVE, 1000);
}

void isPlayerLevelUp(int user_id)
{
	if (g_clients[user_id].exp >= (int)(100 * pow(2, (g_clients[user_id].level - 1))))
	{
		g_clients[user_id].exp = 0;
		g_clients[user_id].hp = 100;
		g_clients[user_id].level += 1;
	}
}


void npc_do_attack(int npc_id)
{
	add_timer(npc_id, OP_NPC_ATTACK, 1000);
}


void isNPCDie(int user_id, int npc_id)
{
	if (g_clients[npc_id].hp <= 0)
	{
		char mess[100];
		if (g_clients[npc_id].npcCharacterType == NPC_WAR || g_clients[npc_id].npcMoveType == NPC_RANDOM_MOVE)
		{
			g_clients[user_id].exp += g_clients[npc_id].level * 5 * 2;
			sprintf_s(mess, "**%s** EXP (+%d).", g_clients[user_id].m_name, g_clients[npc_id].level * 5 * 2);
		}
		else
		{
			g_clients[user_id].exp += g_clients[npc_id].level * 5;
			sprintf_s(mess, "**%s** EXP (+%d).", g_clients[user_id].m_name, g_clients[npc_id].level * 5);
		}

		isPlayerLevelUp(user_id);
		g_clients[npc_id].m_status = ST_ALLOCATED;
		while (true)
		{
			int x = rand() % WORLD_WIDTH;
			int y = rand() % WORLD_HEIGHT;

			if (g_Map[y][x].type == eBLANK)
			{
				g_clients[npc_id].x = x;
				g_clients[npc_id].y = y;
				break;
			}
		}

		//for (int i = 0; i < g_totalUserCount; ++i)
		//	send_leave_packet(i, npc_id);

		for (int i = 0; i < g_totalUserCount; ++i)
			send_chat_packet(i, user_id, mess, 1);

		send_stat_change_packet(user_id, user_id);
		for (auto vl : g_clients[user_id].view_list)
			send_stat_change_packet(vl, user_id);

		add_timer(npc_id, OP_NPC_RESPAWN, 1000);
		g_clients[npc_id].target = nullptr;
	}
}


void random_move_npc(int npc_id)
{
	int x = g_clients[npc_id].x;
	int y = g_clients[npc_id].y;

	if (g_clients[npc_id].target == nullptr && g_clients[npc_id].npcMoveType != NPC_RANDOM_MOVE)
		return;

	if (g_clients[npc_id].target != nullptr)
	{
		bool ret = g_clients[npc_id].pathfind.searchLoad(g_clients[npc_id].mapData,
			x, y, g_clients[npc_id].target->x, g_clients[npc_id].target->y);

		if (ret)
		{
			bool isattack = g_clients[npc_id].pathfind.returnPos(&x, &y);

			if (isattack)
				npc_do_attack(npc_id);


			if (g_clients[npc_id].target != nullptr)
			{
				char mess[100];
				sprintf_s(mess, "I SEE YOU !");
				send_chat_packet(g_clients[npc_id].target->m_id, npc_id, mess, 0);
			}
		}
		else
		{
			char mess[100];
			sprintf_s(mess, "WHERE ARE YOU !");
			send_chat_packet(g_clients[npc_id].target->m_id, npc_id, mess, 0);
		}
	}
	else
	{
		switch (rand() % 4)
		{
		case 0:
			if (g_Map[y][x + 1].type == eBLANK)
			{
				if (x < WORLD_WIDTH - 1)
				{
					x++;
				}
			}
			break;
		case 1:
			if (g_Map[y][x - 1].type == eBLANK)
			{
				if (x > 0)
				{
					x--;
				}
			}
			break;
		case 2:
			if (g_Map[y + 1][x].type == eBLANK)
			{
				if (y < WORLD_HEIGHT - 1)
				{
					y++;
				}
			}
			break;
		case 3:
			if (g_Map[y - 1][x].type == eBLANK)
			{
				if (y > 0)
				{
					y--;
				}
			}
			break;
		}
	}

	g_clients[npc_id].x = x;
	g_clients[npc_id].y = y;

	add_timer(npc_id, OP_RANDOM_MOVE, 1000);


	for (int i = 0; i < NPC_ID_START; ++i)
	{
		if (g_clients[i].m_status != ST_ACTIVE) continue;
		if (true == is_near(i, npc_id))
		{
			g_clients[i].m_cLock.lock();
			if (0 != g_clients[i].view_list.count(npc_id))
			{
				g_clients[i].m_cLock.unlock();
				send_move_packet(i, npc_id);
			}
			else
			{
				g_clients[i].m_cLock.unlock();
				send_enter_packet(i, npc_id);

				if (g_clients[npc_id].target == nullptr)
				{
					g_clients[npc_id].target = &g_clients[i];
					if (g_clients[npc_id].npcMoveType != NPC_RANDOM_MOVE)
						random_move_npc(npc_id);
				}
			}
		}
		else
		{
			g_clients[i].m_cLock.lock();
			if (0 != g_clients[i].view_list.count(npc_id))
			{
				g_clients[i].m_cLock.unlock();
				send_leave_packet(i, npc_id);
			}
			else
				g_clients[i].m_cLock.unlock();
		}
	}

}

void do_attack(int user_id)
{
	g_clients[user_id].m_cLock.lock();
	auto vl = g_clients[user_id].view_list;
	g_clients[user_id].m_cLock.unlock();

	for (auto npc : vl)
	{
		if (g_clients[npc].m_id < NPC_ID_START)
			continue;

		if ((g_clients[npc].x == g_clients[user_id].x && g_clients[npc].y == g_clients[user_id].y - 1) ||
			(g_clients[npc].x == g_clients[user_id].x && g_clients[npc].y == g_clients[user_id].y + 1) ||
			(g_clients[npc].x == g_clients[user_id].x - 1 && g_clients[npc].y == g_clients[user_id].y) ||
			(g_clients[npc].x == g_clients[user_id].x + 1 && g_clients[npc].y == g_clients[user_id].y))
		{

			g_clients[npc].hp -= 2 * g_clients[user_id].level;

			if (g_clients[npc].target == nullptr)
			{
				g_clients[npc].target = &g_clients[user_id];
				if (g_clients[npc].npcMoveType != NPC_RANDOM_MOVE)
					random_move_npc(npc);
			}

			if(g_clients[npc].hp < 0) g_clients[npc].hp = 0;
			isNPCDie(user_id, npc);

			char mess[100];
			sprintf_s(mess, "%s -> attack -> NPC %d (-%d).", g_clients[user_id].m_name, npc, g_clients[user_id].level * 2);

			for (int i=0; i< g_totalUserCount; ++i)
			{
				send_stat_change_packet(i, npc);
				send_chat_packet(i, user_id, mess, 1);
			}
		}
	}
}

void do_move(int user_id, int direction, bool isDirect, int _Directx = 0, int _Directy = 0)
{
	int x = g_clients[user_id].x;
	int y = g_clients[user_id].y;

	if (!isDirect)
	{
		switch (direction)
		{
		case D_UP:
			if (g_Map[y-1][x].type == eBLANK)
			{
				if (y > 0)
				{
					y--;
				}
			}
			break;
		case D_DOWN:
			if (g_Map[y+1][x].type == eBLANK)
			{
				if (y < WORLD_HEIGHT - 1)
				{
					y++;
				}
			}
			break;
		case D_LEFT:
			if (g_Map[y][x-1].type == eBLANK)
			{
				if (x > 0)
				{
					x--;
				}
			}
			break;
		case D_RIGHT:
			if (g_Map[y][x+1].type == eBLANK)
			{
				if (x < WORLD_WIDTH - 1)
				{
					x++;
				}
			}
			break;
		default:
			cout << "unknown direction from client move packet \n";
			DebugBreak();
		}
	}
	else
	{
		x = _Directx;
		y = _Directy;
	}

	g_clients[user_id].x = x;
	g_clients[user_id].y = y;

	//복사해두고 쓴다. 근데 그 이후에 어긋나더라도 그건 감수해야한다. 지금은 이정도로 만족하자
	g_clients[user_id].m_cLock.lock();
	unordered_set<int> old_vl = g_clients[user_id].view_list;
	g_clients[user_id].m_cLock.unlock();
	
	unordered_set<int> new_vl;
	for (auto &cl : g_clients)
	{
		if (false == is_near(cl.m_id, user_id)) continue;		//모든 클라 다 깨우는게 아니라 근처에 있는 애만 깨우기 
		if (ST_SLEEP == cl.m_status)
		{
			cl.m_status = ST_ACTIVE;
			//플레이어가 npc 시야에 들어오면 1초마다 길찾기 하면서 쫓아옴
			if (cl.npcCharacterType == NPC_WAR)
			{
				if (cl.target == nullptr)
				{
					cl.target = &g_clients[user_id];
					if (g_clients[cl.m_id].npcMoveType != NPC_RANDOM_MOVE)
						random_move_npc(cl.m_id);
				}
			}
			//activate_npc(cl.m_id);		//잠들어 있던 npc면 active로 바꾸기
		}	
		if (ST_ACTIVE != cl.m_status) continue;					//그 외 상태는 빠져나가기
		if (cl.m_id == user_id) continue;						//나 자신이면 빠져나가기
		
		//ncp라면 플레이어가 이동했다고 OP_PLAYER_MOVE를 보내준다. 
		if (false == is_player(cl.m_id))
		{
			EXOVER* over = new EXOVER();
			over->p_id = user_id;
			over->op = OP_PLAYER_MOVE;
			PostQueuedCompletionStatus(g_iocp, 1, cl.m_id, &over->over);
		}
		new_vl.insert(cl.m_id);									//새로운 view list에 삽입
	}

	//밑에서 view list로 알려주니까 나한테는 안 알려줌. 그래서 지금 먼저 나한테 이동을 알려줘야 함.
	send_move_packet(user_id, user_id);

	//시야에 새로 들어온 플레이어 
	for (auto newPlayer : new_vl)
	{
		//old_vl에 없었는데 new_vl에 있다면 새로 생긴 것.
		if (0 == old_vl.count(newPlayer))
		{
			send_enter_packet(user_id, newPlayer);

			if (false == is_player(newPlayer) && g_clients[newPlayer].npcCharacterType == NPC_WAR)
			{
				g_clients[newPlayer].target = &g_clients[user_id];
				continue;
			}

			g_clients[newPlayer].m_cLock.lock();
			if (0 == g_clients[newPlayer].view_list.count(user_id)) //멀티쓰레드 프로그램이니까, 다른 스레드에서 미리 시야 처리를 했을 수 있다.
			{
				g_clients[newPlayer].m_cLock.unlock();
				send_enter_packet(newPlayer, user_id);
			}
			else
			{
				g_clients[newPlayer].m_cLock.unlock();
				send_move_packet(newPlayer, user_id);
			}
		}
		//새로 들어온 플레이어가 아니라 옛날에도 보였던 애라면 이동을 알려줌
		else
		{
			if (false == is_player(newPlayer))
			{
				continue;
			}
			//상대방이 이동하면서 시야에서 나를 뺐을 수 있다. 그러니까 상대방 viewlist를 확인하고 보내야 한다.
			g_clients[newPlayer].m_cLock.lock();
			if (0 != g_clients[newPlayer].view_list.count(user_id))
			{
				g_clients[newPlayer].m_cLock.unlock();
				send_move_packet(newPlayer, user_id);
			}
			else
			{
				g_clients[newPlayer].m_cLock.unlock();
				send_enter_packet(newPlayer, user_id);
			}
		}
	}


	//시야에서 벗어난 플레이어
	for (auto oldPlayer : old_vl)
	{

		if (0 == new_vl.count(oldPlayer))
		{
			send_leave_packet(user_id, oldPlayer);

			if (false == is_player(oldPlayer))
			{
				//플레이어가 npc 시야에 벗어나면 안쫓아옴
				if (oldPlayer >= NPC_ID_START)
					g_clients[oldPlayer].target = nullptr;
				continue;
			}
	
			g_clients[oldPlayer].m_cLock.lock();
			if (0 != g_clients[oldPlayer].view_list.count(user_id))
			{
				g_clients[oldPlayer].m_cLock.unlock();
				send_leave_packet(oldPlayer, user_id);
			}
			else
				g_clients[oldPlayer].m_cLock.unlock();
		}
	}
}

bool isPlayerDie(int user_id)
{
	if (g_clients[user_id].hp <= 0)
	{
		return true;
	}
	else
		return false;
}


void enter_game(int user_id, char name[])
{
	g_clients[user_id].m_cLock.lock();

	strcpy_s(g_clients[user_id].m_name, name);
	g_clients[user_id].m_name[MAX_ID_LEN] = NULL;
	send_login_ok_packet(user_id);

	if (g_clients[user_id].hp < 100)
		add_timer(user_id, OP_PLAYER_HP_HEAL, 5000);
	
	g_clients[user_id].m_status = ST_ACTIVE;

	g_clients[user_id].m_cLock.unlock();

	for (auto& cl : g_clients)
	{
		int i = cl.m_id;
		if (user_id == i) continue;
		//시야를 벗어났으면 처리하지 말아라 (입장 시 시야처리)
		if (true == is_near(user_id, i))
		{
			//npc가 자고있다면 깨우기
			if (ST_SLEEP == g_clients[i].m_status)
				cl.m_status = ST_ACTIVE;//activate_npc(i);
		
			//g_clients[i].m_cLock.lock(); //i와 user_id가 같아지는 경우 2중락으로 인한 오류 발생 (데드락)
			//다른 곳에서 status를 바꿀 수 있지만, 실습에서 사용할정도의 복잡도만 유지해야 하기 때문에 일단 놔두기로 한다. 
			//그래도 일단 컴파일러 문제랑 메모리 문제는 해결해야 하기 때문에 status 선언을 atomic으로 바꾼다. 
			if (ST_ACTIVE == g_clients[i].m_status)
			{
				send_enter_packet(user_id, i);
				if(true == is_player(i)) //플레이어인 경우만 send. 안그러면 소켓이 없어서 에러남
					send_enter_packet(i, user_id); //니가 나를 보면 나도 너를 본다
				else
				{
					//플레이어가 npc 시야에 들어오면 1초마다 길찾기 하면서 쫓아옴
					if (cl.npcCharacterType == NPC_WAR)
					{
						if (g_clients[user_id].target == nullptr)
						{
							cl.target = &g_clients[user_id];

							if (g_clients[cl.m_id].npcMoveType != NPC_RANDOM_MOVE)
								random_move_npc(cl.m_id);
						}
					}
				}
			}
			//g_clients[i].m_cLock.unlock();
		}
	}
}

void process_packet(int user_id, char* buf)
{
	switch (buf[1]) //[0]은 size
	{
	case C2S_LOGIN:
	{	
		cs_packet_login *packet = reinterpret_cast<cs_packet_login*>(buf);
		//cout << "<" << packet->name << ">" << endl;

		for (auto dblist : g_dbData)
		{
			if (strcmp(packet->name, dblist.Name) == 0)
			{
				g_clients[user_id].x = dblist.x;
				g_clients[user_id].y = dblist.y;
				g_clients[user_id].m_db = dblist.id;
				g_clients[user_id].exp = dblist.exp;
				g_clients[user_id].hp = dblist.HP;
				g_clients[user_id].level = dblist.level;
				memcpy(g_clients[user_id].m_name, packet->name, 10);
				enter_game(user_id, packet->name);
				return;
			}
		}

		//id가 없다면 새로 생성 
		//if (strcmp(packet->name, "") == 0)
		{
			int x;
			int y;
			while (true)
			{
				x = rand() % WORLD_WIDTH;
				y = rand() % WORLD_HEIGHT;

				if (g_Map[y][x].type == eBLANK)
					break;
			}

			g_totalUserCount++;
			InsertData(g_totalUserCount, packet->name, x, y, 1, 0, 100);
			g_clients[user_id].x = x;
			g_clients[user_id].y = y;
			g_clients[user_id].m_db = g_totalUserCount;
			g_clients[user_id].exp = 0;
			g_clients[user_id].hp = 100;
			g_clients[user_id].level = 1;
			sprintf_s(g_clients[user_id].m_name, "USER%d", g_totalUserCount);
			enter_game(user_id, packet->name);
		}


		//closesocket(g_clients[user_id].m_socket);
	
		break;
	}
	case C2S_MOVE:
	{	cs_packet_move *packet = reinterpret_cast<cs_packet_move*>(buf);
		g_clients[user_id].m_move_time = packet->move_time;
		do_move(user_id, packet->direction, false);
		break;
	}
	case C2S_ATTACK:
	{
		cs_packet_attack *packet = reinterpret_cast<cs_packet_attack*>(buf);
		do_attack(user_id);
		break;
	}
	default:
		cout << "unknown packet type error \n";
		DebugBreak(); 
		exit(-1);
	}
}

void initialize_clients()
{
	for (int i = 0; i < MAX_USER; ++i)
	{
		//이건 멀티쓰레드로 돌기 전에, 싱글쓰레드로 돌아가는 함수여서 lock을 거는 의미가 없음. 
		g_clients[i].m_status = ST_FREE;
		g_clients[i].m_id = i;
	}
}


void disconnect(int user_id)
{
	send_leave_packet(user_id, user_id); //나는 나에게 보내기
	
	g_clients[user_id].m_cLock.lock();
	g_clients[user_id].m_status = ST_ALLOCATED;	//처리 되기 전에 FREE하면 아직 떠나는 뒷처리가 안됐는데 새 접속을 받을 수 있음

	closesocket(g_clients[user_id].m_socket);

	for (int i = 0; i < NPC_ID_START; ++i)
	{
		CLIENT& cl = g_clients[i];
		if (cl.m_id == user_id) continue;
		//cl.m_cLock.lock();
		if (ST_ACTIVE == cl.m_status)
			send_leave_packet(cl.m_id, user_id);
		//cl.m_cLock.unlock();
	}

	for (auto vl : g_clients[user_id].view_list)
	{
		g_clients[vl].target = nullptr;
	}

	g_clients[user_id].m_status = ST_FREE;	//다 처리했으면 FREE
	g_clients[user_id].m_cLock.unlock();
}

//패킷 재조립 - 유저에게, 바이트만큼의 데이터가 왔다
void packet_construct(int user_id, int io_byte)
{
	CLIENT &curr_user = g_clients[user_id];
	EXOVER &recv_over = curr_user.m_recv_over;

	int rest_byte = io_byte;		//이만큼 남은걸 처리해줘야 한다
	char *p = recv_over.io_buf;		//처리해야할 데이터의 포인터가 필요하다
	int packet_size = 0;			//이게 0이라는 것은 이전에 처리하던 패킷이 없다는 것 
	
	if (curr_user.m_prev_size != 0)	
		packet_size = curr_user.m_packet_buf[0]; //재조립을 기다기는 패킷 사이즈
	
	while (rest_byte > 0)	//처리해야할 데이터가 남아있으면 처리해야한다.
	{
		if (0 == packet_size)	packet_size = p[0];

		//나머지 데이터로 패킷을 만들 수 있나 없나 확인
		if (packet_size <= rest_byte + curr_user.m_prev_size)
		{
			memcpy(curr_user.m_packet_buf + curr_user.m_prev_size, p, packet_size - curr_user.m_prev_size);		//만들어서 처리한 데이터 크기만큼 패킷 사이즈에서 빼주기
			
			p += packet_size - curr_user.m_prev_size;
			rest_byte -= packet_size - curr_user.m_prev_size;
			packet_size = 0;														//이 패킷은 이미 처리를 했고 다음 패킷 사이즈는 모름.

			process_packet(user_id, curr_user.m_packet_buf);
			
			curr_user.m_prev_size = 0;

		}
		else	//패킷 하나를 만들 수 없다면 버퍼에 복사해두고 포인터와 사이즈 증가
		{
			memcpy(curr_user.m_packet_buf + curr_user.m_prev_size, p, rest_byte); //남은 데이터 몽땅 받는데, 지난번에 받은 데이터가 남아있을 경우가 있으니, 그 뒤에 받아야한다.
			curr_user.m_prev_size += rest_byte;
			rest_byte = 0;
			p += rest_byte;
		}
	}
}

void worker_Thread()
{
	while (true) {

		DWORD io_byte;
		ULONG_PTR key;
		WSAOVERLAPPED* over;
		GetQueuedCompletionStatus(g_iocp, &io_byte, &key, &over, INFINITE);

		EXOVER *exover = reinterpret_cast<EXOVER*>(over);
		int user_id = static_cast<int>(key);

		CLIENT& cl = g_clients[user_id]; //타이핑 줄이기 위해

		switch (exover->op)
		{
		case OP_RECV:			//받은 패킷 처리 -> overlapped구조체 초기화 -> recv
		{
			if (0 == io_byte)
			{
				disconnect(user_id);

				UpdateData(g_clients[user_id].m_db, g_clients[user_id].x, g_clients[user_id].y, g_clients[user_id].level, g_clients[user_id].exp, g_clients[user_id].hp);
				
				if (OP_SEND == exover->op)
					delete exover;
			}
			else
			{
				packet_construct(user_id, io_byte);
				ZeroMemory(&cl.m_recv_over.over, 0, sizeof(cl.m_recv_over.over));
				DWORD flags = 0;
				WSARecv(cl.m_socket, &cl.m_recv_over.wsabuf, 1, NULL, &flags, &cl.m_recv_over.over, NULL);;
			}
			break;
		}
		case OP_SEND:			//구조체 delete
			if (0 == io_byte)
				disconnect(user_id);

			delete exover;
			break;

		case OP_ACCEPT:			//CreateIoCompletionPort으로 클라소켓 iocp에 등록 -> 초기화 -> recv -> accept 다시(다중접속)
		{
			int user_id = -1;
			for (int i = 0; i < MAX_USER; ++i)
			{
				lock_guard<mutex> gl{ g_clients[i].m_cLock }; //이렇게 하면 unlock이 필요 없다. 이 블록에서 빠져나갈때 unlock을 자동으로 해준다.
				if (ST_FREE == g_clients[i].m_status )
				{
					g_clients[i].m_status = ST_ALLOCATED;
					user_id = i;
					break;
				}
			}

			//main에서 소켓을 worker스레드로 옮겨오기 위해 listen소켓은 전역변수로, client소켓은 멤버로 가져왔다.
			SOCKET clientSocket = exover->c_socket;
			
			if (-1 == user_id)
				closesocket(clientSocket); // send_login_fail_packet();
			else
			{
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(clientSocket), g_iocp, user_id, 0);

				//g_clients[user_id].m_id = user_id; 멀쓰에서 하는게 아니고 초기화 할때 한번 해줘야 함 처음에 한번.
				g_clients[user_id].m_prev_size = 0; //이전에 받아둔 조각이 없으니 0
				g_clients[user_id].m_socket = clientSocket;

				ZeroMemory(&g_clients[user_id].m_recv_over.over, 0, sizeof(g_clients[user_id].m_recv_over.over));
				g_clients[user_id].m_recv_over.op = OP_RECV;
				g_clients[user_id].m_recv_over.wsabuf.buf = g_clients[user_id].m_recv_over.io_buf;
				g_clients[user_id].m_recv_over.wsabuf.len = MAX_BUF_SIZE;

				g_clients[user_id].x = rand() % WORLD_WIDTH;
				g_clients[user_id].y = rand() % WORLD_HEIGHT;

				g_clients[user_id].view_list.clear();

				DWORD flags = 0;
				WSARecv(clientSocket, &g_clients[user_id].m_recv_over.wsabuf, 1, NULL, &flags, &g_clients[user_id].m_recv_over.over, NULL);
			}

			//소켓 초기화 후 다시 accept
			clientSocket = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			exover->c_socket = clientSocket; //새로 받는 소켓을 넣어준다. 안그러면 클라들이 같은 소켓을 공유한다.
			ZeroMemory(&exover->over, sizeof(exover->over)); //accept_over를 exover라는 이름으로 받았으니 exover를 재사용
			AcceptEx(listenSocket, clientSocket, exover->io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &exover->over);

		}
			break;

		case OP_RANDOM_MOVE:
		{
			random_move_npc(user_id);

			delete exover;
		}
			break;

		case OP_PLAYER_MOVE:
		{
			g_clients[user_id].lua_l.lock();
			lua_State* L = g_clients[user_id].L;
			lua_getglobal(L, "event_player_move");
			lua_pushnumber(L, exover->p_id);//누가 움직였는지 받기 위해 exover에 union을 이용한다.
			int err = lua_pcall(L, 1, 0, 0);
			g_clients[user_id].lua_l.unlock();

			delete exover;
		}
			break;
		case OP_PLAYER_HP_HEAL:
		{
			//플레이어 hp 5초마다 10씩 회복
			g_clients[user_id].hp += 10;
			if (g_clients[user_id].hp >= 100)
				g_clients[user_id].hp = 100;
			else if (g_clients[user_id].hp < 100)
				add_timer(user_id, OP_PLAYER_HP_HEAL, 5000);

			send_stat_change_packet(user_id, user_id);

			g_clients[user_id].m_cLock.lock();
			unordered_set<int> vl = g_clients[user_id].view_list;
			g_clients[user_id].m_cLock.unlock();

			char mess[100];
			sprintf_s(mess, "USER%d HP(+10)",user_id);

			for (auto vlPlayer : vl)
			{
				send_stat_change_packet(vlPlayer, user_id);
				send_chat_packet(vlPlayer, user_id, mess, 1);
			}
		}
		break;
		case OP_NPC_RESPAWN:
		{
			g_clients[user_id].hp = 100;
			if (g_clients[user_id].npcMoveType == 1)
				add_timer(user_id, OP_RANDOM_MOVE, 1000); //그냥 움직이는게 아니라 플레이어가 움직였을때 시야에 들어오면 깨워야 함
			
			g_clients[user_id].target = nullptr;
			g_clients[user_id].m_status = ST_SLEEP;

		}
			break;
		case OP_NPC_ATTACK:
		{
			if (g_clients[user_id].target == nullptr)
				break;

			int target_id = g_clients[user_id].target->m_id;

			if (g_clients[target_id].hp == 100)
			{
				add_timer(target_id, OP_PLAYER_HP_HEAL, 5000);
			}
			g_clients[target_id].hp -= g_clients[user_id].level;
			char mess[100];
			sprintf_s(mess, "NPC %d -> attack -> USER %d (-%d)", user_id, g_clients[target_id].m_id, g_clients[user_id].level);
			
			for (int i = 0; i < g_totalUserCount; ++i)
				send_chat_packet(i, target_id, mess, 1);

			bool isdie = isPlayerDie(target_id);

			if (isdie)
			{

				g_clients[target_id].exp /= 2;
				g_clients[target_id].hp = 100;
				do_move(g_clients[target_id].m_id, 0, true, 401, 400);

				for (int i = 0; i < g_totalUserCount; ++i)
					send_stat_change_packet(i, g_clients[target_id].m_id);
				
				char mess1[100];
				sprintf_s(mess1, "!!! RESPAWN !!!");
				send_chat_packet(g_clients[target_id].m_id, g_clients[target_id].m_id,mess1, 2);
				
				g_clients[user_id].target = nullptr;
			}
			else if (!isdie)
			{
				for (int i = 0; i < g_totalUserCount; ++i)
					send_stat_change_packet(i, g_clients[target_id].m_id);
			}
		}
			break;
		default:
			cout << "unknown operation in worker_thread\n";
			while (true);
		}
	}

}

int API_SendMessageHELLO(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* message = (char*)lua_tostring(L, -1);

	send_chat_packet(user_id, my_id, message,0);
	g_clients[my_id].moveCount = 0;

	lua_pop(L, 3);
	return 0; //리턴 값 개수 0개
}

int API_SendMessageBYE(lua_State* L)
{
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* message = (char*)lua_tostring(L, -1);

	send_chat_packet(user_id, my_id, message,0);

	lua_pop(L, 3);
	return 0; //리턴 값 개수 0개
}

int API_get_x(lua_State* L)
{
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = g_clients[obj_id].x;
	lua_pushnumber(L, x);
	return 1;
}

int API_get_y(lua_State* L)
{
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = g_clients[obj_id].y;
	lua_pushnumber(L, y);
	return 1;
}

void init_npc()
{
	for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i)
	{
		g_clients[i].m_socket = 0;
		g_clients[i].m_id = i;
		sprintf_s(g_clients[i].m_name, "NPC%d", i);
		g_clients[i].m_status = ST_SLEEP;

		while (true)
		{
			int x = rand() % WORLD_WIDTH;
			int y = rand() % WORLD_HEIGHT;

			if (g_Map[y][x].type == eBLANK)
			{
				g_clients[i].x = x;
				g_clients[i].y = y;
				break;
			}
		}

		g_clients[i].level = rand() % 5 + 1;
		g_clients[i].hp = 100;

		g_clients[i].moveCount = 0;
		g_clients[i].npcCharacterType = rand() % 2;
		g_clients[i].npcMoveType = rand() % 2;

		g_clients[i].mapData = (int**)malloc(sizeof(int*) * WORLD_HEIGHT);
		for (int j = 0; j < WORLD_HEIGHT; j++) {
			g_clients[i].mapData[j] = (int*)malloc(sizeof(int) * WORLD_WIDTH);
		}

		//g_clients[i].m_last_move_time = high_resolution_clock::now();
		if(g_clients[i].npcMoveType == NPC_RANDOM_MOVE)
			add_timer(i, OP_RANDOM_MOVE, 1000); //그냥 움직이는게 아니라 플레이어가 움직였을때 시야에 들어오면 깨워야 함


		lua_State *L = g_clients[i].L = luaL_newstate();
		luaL_openlibs(L);
		luaL_loadfile(L, "NPC.LUA");
		lua_pcall(L, 0, 0, 0);
		lua_getglobal(L, "set_uid");
		lua_pushnumber(L, i);
		lua_pcall(L, 1, 0, 0);
		lua_pop(L, 1);

		//API 등록
		lua_register(L, "API_SendMessageHELLO", API_SendMessageHELLO);
		lua_register(L, "API_SendMessageBYE", API_SendMessageBYE);
		lua_register(L, "API_get_x", API_get_x);
		lua_register(L, "API_get_y", API_get_y);
		//lua_register(L, "activate_npc", activate_npc);
	}
}


void do_timer()
{
	while (true)
	{
		this_thread::sleep_for(1ms); //Sleep(1);
		while (true)
		{
			timer_lock.lock();
			if (timer_queue.empty() == true)
			{
				timer_lock.unlock();
				break;
			}
			if (timer_queue.top().wakeup_time > high_resolution_clock::now())
			{
				timer_lock.unlock();
				break;
			}
			event_type ev = timer_queue.top();
			timer_queue.pop();
			timer_lock.unlock();

			switch (ev.event_id)
			{
			case OP_RANDOM_MOVE:
			{	EXOVER* over = new EXOVER();
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;
			case OP_PLAYER_HP_HEAL:
			{
				EXOVER* over = new EXOVER();
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;
			case OP_NPC_RESPAWN:
			{
				EXOVER* over = new EXOVER();
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;
			case OP_NPC_ATTACK:
			{
				EXOVER* over = new EXOVER();
				over->op = ev.event_id;
				PostQueuedCompletionStatus(g_iocp, 1, ev.obj_id, &over->over);
			}
			break;
			}
		}
	}
}


void init_map()
{
	char data;
	FILE* fp = fopen("mapData.txt", "rb");
	
	int count = 0;
	
	while (fscanf(fp, "%c", &data) != EOF) {
		//printf("%c", data);
		switch (data)
		{
		case '0':
			g_Map[count / 800][count % 800].type = eBLANK;
			count++;
			break;
		case '3':
			g_Map[count / 800][count % 800].type = eBLOCKED;
			count++;
			break;
		default:
			break;
		}
	}
	fclose(fp);
	
	//for (int i = 0; i < WORLD_HEIGHT; ++i)
	//{
	//	for (int j = 0; j < WORLD_WIDTH; ++j)
	//	{
	//		g_Map[i][j].type = eBLANK;
	//	}
	//}
}

void main()
{
	//네트워크 초기화
	WSADATA WSAData;
	WSAStartup(MAKEWORD(2, 2), &WSAData);
	
	cout << "map initialization start \n";
	init_map();
	cout << "map initialization finished \n";

	cout << "npc initialization start \n";
	init_npc();
	cout << "npc initialization finished \n";

	cout << "DB data initialization start \n";
	LoadData();
	cout << "DB data initialization finished \n";

	
	//맨 뒤에 flag를 overlapped로 꼭 줘야 제대로 동작
	listenSocket = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	//bind에 사용할 server address
	SOCKADDR_IN serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT); //host to network 해서 넣어야 한다
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY); //모든 클라로부터 접속을 받아야 한다
	::bind(listenSocket, reinterpret_cast<SOCKADDR *>(&serverAddr), sizeof(serverAddr)); //그냥 bind를 쓰면 c++11에 있는 키워드로 연결된다. 따라서 앞에 ::를 붙인다.

	listen(listenSocket, SOMAXCONN);

	//IOCP 핸들 할당, IOCP객체 만들기
	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);

	initialize_clients();


	//listen소켓 등록
	CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSocket), g_iocp, 999, 0);

	//원래 accept는 소켓을 리턴하는데, 이 함수는 미리 소켓을 만들어 두고 그 소켓을 클라와 연결시켜준다. 동작이 좀 다름.
	//비동기로 acceptEx호출. 
	SOCKET clientSocket = WSASocketW(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	EXOVER accept_over; 
	ZeroMemory(&accept_over, sizeof(accept_over.over));
	accept_over.op = OP_ACCEPT;
	accept_over.c_socket = clientSocket;
	AcceptEx(listenSocket, clientSocket, accept_over.io_buf , NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &accept_over.over);
	
	//스레드 만들기
	vector <thread> worker_threads;
	for (int i = 0; i < 5; ++i)
		worker_threads.emplace_back(worker_Thread);

	thread timer_thread{ do_timer };

	//메인 종료 전 모든 스레드 종료 기다리기
	for (auto &th : worker_threads) th.join();
	timer_thread.join();
}