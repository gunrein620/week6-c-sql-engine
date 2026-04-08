# C SQL Engine

> C 언어로 구현한 파일 기반 SQL 처리기

---

## 프로젝트 개요

- 대상: `members` 테이블
- 목적: SQL 문장 직접 해석 및 실행
- 지원 범위: `INSERT`, `SELECT`
- 저장 방식: `.schema`, `.tbl`


## 핵심 데이터 구조

- 기준 도메인: `members`
- 핵심 구조체: `Token`, `Statement`, `MemberRecord`

```c
typedef struct {
    TokenType type;
    char value[MAX_TOKEN_LEN];
    int line;
} Token;

typedef struct {
    StatementType type;
    char table_name[MAX_IDENTIFIER_LEN];
    ColumnList select_columns;
    WhereClause where;
    ColumnList insert_columns;
    char values[MAX_COLUMNS][MAX_TOKEN_LEN];
    int value_is_null[MAX_COLUMNS];
    int value_count;
} Statement;

typedef struct {
    int32_t id;
    char name[MAX_NAME_LEN];
    char grade[MAX_GRADE_LEN];
    char class[MAX_CLASS_LEN];
    int32_t age;
} MemberRecord;
```

- `Token`: SQL 최소 단위
- `Statement`: Parser 결과 구조
- `MemberRecord`: `members` 테이블 기준 레코드
- 문자열 길이 기준: `MAX_NAME_LEN`, `MAX_GRADE_LEN`, `MAX_CLASS_LEN`

---

## SQL 처리 파이프라인

- 입력 방식: `-f`, `-e`
- 문장 분리: 세미콜론 기준
- 예외 처리: 문자열 내부 세미콜론, `--` 주석 제외

```text
SQL 입력
  -> tokenize()
  -> parse()
  -> execute()
      -> schema_load()
      -> storage_insert() / storage_select()
```

### 실행 단계

| 단계 | 함수 | 역할 |
|------|------|------|
| 입력 분해 | `tokenize()` | SQL 문자열 → Token 배열 |
| 문법 해석 | `parse()` | Token 배열 → Statement |
| 실행 분기 | `execute()` | INSERT / SELECT 분기 실행 |
| 스키마 검증 | `schema_load()` | `schemas/{table}.schema` 로드 |
| 파일 저장/조회 | `storage_insert()`, `storage_select()` | `.tbl` 저장 및 조회 |

## 주요 쟁점

### 1. 키워드 대소문자 처리 vs 식별자 정규화

- 키워드 처리: 대소문자 무시
- 식별자 처리: 소문자 정규화
- 목적: Parser / Schema / Storage 비교 로직 단순화

### 2. WHERE 처리 방식 — 평면 조건 배열

- 저장 방식: `Condition[] + logical_op`
- 장점: 구현 단순화

```c
typedef struct {
    Condition conditions[MAX_CONDITIONS];
    int condition_count;
    char logical_op[4];
} WhereClause;
```

### 3. 파일 기반 저장 방식

- 저장 포맷: 파이프(`|`) 구분 텍스트
- 장점: 구조 확인 쉬움

```text
id|name|grade|class|age
1|Alice|vip|advanced|30
2|Bob|normal|basic|22
```

### 4. Executor 중심 검증 로직

- 검증 항목:
  - 컬럼 수 / 값 수 일치
  - 존재하지 않는 컬럼명
  - 중복 컬럼
  - 타입 검증
  - VARCHAR 길이 초과
  - NULL 허용 여부
  - 기본키 중복
  - 저장 불가 문자 포함 여부

- 역할:
  - 입력 검증
  - 데이터 무결성 보장
  - storage 호출 전 최종 방어선

### 5. 아키텍처 비교 — 우리 프로젝트 vs MySQL vs SQLite

| 항목 | 우리 프로젝트 | MySQL | SQLite |
|------|-------------|-------|--------|
| 목표 | SQL 처리 흐름 학습 | 범용 서버형 RDBMS | 임베디드 DB 엔진 |
| 실행 형태 | 단일 CLI 프로그램 | 클라이언트-서버 | 라이브러리 + CLI 셸 |
| 처리 파이프라인 | CLI -> Lexer -> Parser -> Executor -> Storage/Schema | Parser -> Optimizer -> Executor -> Storage Engine | Tokenizer -> Parser -> Planner/CodeGen -> VDBE -> B-tree/Pager |
| 실행 계획 최적화 | 없음 | 강력함 | 있음 |
| 저장 방식 | 텍스트 `.tbl` | InnoDB 등 엔진 | 단일 DB 파일 |
| 트랜잭션 | 없음 | 지원 | 지원 |
| 장애 복구 | 없음 | redo / undo 기반 | rollback journal / WAL |
| 인덱스 | 없음 | 지원 | 지원 |

- 포지셔닝: 산업용 DBMS 재현 아님
- 비교 기준: 구조 학습용 mini SQL engine
- 차별점: SQL 입력부터 저장까지 흐름이 눈에 보이는 단순 구조
- 한계: 최적화, 복구, 동시성, 인덱스는 미구현

---

## 테스트케이스 및 엣지케이스

- 테스트 구성: 모듈별 단위 테스트
- 포함 범위: Lexer, Parser, Schema, Storage, Executor, CLI

**기본 테스트** — 핵심 동작 검증

| 구분 | 케이스 |
|------|--------|
| Lexer | 빈 입력, 대소문자 무시, 문자열 이스케이프, 주석 처리 |
| Parser | `SELECT *`, WHERE AND, INSERT NULL 값, FROM 누락 오류 |
| Schema | members 스키마 로드, 컬럼 조회, 타입 파싱 |
| Storage | 첫 INSERT 시 헤더 생성, WHERE 기반 필터링 |
| Executor | PK 중복 차단, VARCHAR 길이 초과 차단, 잘못된 SELECT 컬럼 차단 |
| CLI | `-e`, `-f` 실행, 파싱 오류 종료 코드 확인 |

**엣지 케이스** — 경계 상황 검증

| 구분 | 케이스 |
|------|--------|
| NULL 처리 | nullable 컬럼에서만 허용 |
| WHERE 제약 | 하나의 WHERE 절에서 AND/OR 혼합 불가 |
| 저장 제약 | `|`, 개행 문자가 포함된 값 저장 불가 |
| 타입 검증 | INT, FLOAT, DATE 형식 검사 |
| 빈 테이블 조회 | 데이터 파일이 없어도 빈 결과 반환 |
