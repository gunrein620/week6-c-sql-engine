# C SQL Engine

> C 언어로 구현한 파일 기반 SQL 처리기

---

## 프로젝트 개요

- 대상: `members` 테이블
- 목적: SQL 문장 직접 해석 및 실행
- 지원 범위: `INSERT`, `SELECT`, 단일 컬럼 `ORDER BY`
- 저장 방식: `.schema`, `.tbl`
- 보강 사항: NULL 보존, 파일 무결성 검증, 기존 PK 중복 탐지, UTF-8 길이 검증, Windows 실행/테스트 대응


## 핵심 데이터 구조

- 기준 도메인: `members`
- 핵심 구조체: `Token`, `Statement`, `Row`, `Condition`, `MemberRecord`

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
    OrderByClause order_by;
    ColumnList insert_columns;
    char values[MAX_COLUMNS][MAX_TOKEN_LEN];
    int value_is_null[MAX_COLUMNS];
    int value_count;
} Statement;

typedef struct {
    char data[MAX_COLUMNS][MAX_TOKEN_LEN];
    int is_null[MAX_COLUMNS];
    int column_count;
} Row;

typedef struct {
    char column_name[MAX_IDENTIFIER_LEN];
    char operator[4];
    char value[MAX_TOKEN_LEN];
    int value_is_null;
} Condition;

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
- `Row`: 저장/조회 시 실제 레코드 표현
- `Condition`: WHERE 조건 + NULL 비교 정보
- `MemberRecord`: `members` 테이블 기준 레코드
- 문자열 길이 기준: `MAX_NAME_LEN`, `MAX_GRADE_LEN`, `MAX_CLASS_LEN`

---

## SQL 처리 파이프라인

- 입력 방식: `-f`, `-e`
- 문장 분리: 세미콜론 기준
- 예외 처리: 문자열 내부 세미콜론, `--` 주석 제외
- 실행 정책: 스크립트 중간 문장 실패 시 즉시 중단

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
- 보강: `= NULL`, `!= NULL` 비교 지원
- 제약: AND/OR 혼합 불가, 괄호식 미지원

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
- 특징: 실제 해석 기준은 `.schema`, NULL은 `\N`으로 저장
- 보강: 헤더 검증, 필드 수 검증, 긴 행/깨진 행 차단, 빈 `.tbl` 차단, 기존 PK 중복 탐지
- 제약: 결과는 메모리 배열(`MAX_ROWS`) 안에서만 관리

```text
id|name|grade|class|age
1|Alice|vip|advanced|30
2|Bob|normal|basic|22
3|NullGuy|\N|basic|20
4|Backslash|\\N|basic|21
```

### 4. Executor 중심 검증 로직

- 검증 항목:
  - 컬럼 수 / 값 수 일치
  - 존재하지 않는 컬럼명
  - 중복 컬럼
  - 타입 검증
  - UTF-8 문자 수 기준 VARCHAR 길이 초과
  - NULL 허용 여부
  - 기본키 중복
  - 저장 불가 문자 포함 여부

- 역할:
  - 입력 검증
  - 데이터 무결성 보장
  - ORDER BY 정렬 처리
  - storage 호출 전 최종 방어선
  - PK 중복 검사 시 기존 파일 full scan 수행

### 5. 안정성 보강 포인트

- 숫자 처리: 음수 / 부호 / 지수 표현 인식
- 저장 포맷: NULL과 빈 문자열 구분 보존
- 스키마 검증: 중복 컬럼명, 다중 PK 차단, 빈 `.schema` 차단
- 조회 안정성: 잘못된 WHERE 컬럼, 깨진 `.tbl` 데이터, 빈 `.tbl`, 기존 PK 중복 즉시 에러
- 문자열 길이: `strlen` 바이트 수가 아니라 UTF-8 문자 수 기준으로 검증
- 실행 일관성: 스크립트 중간 실패 시 이후 문장 실행 중단

### 6. ORDER BY 처리 방식

- 지원 범위: 단일 컬럼 정렬
- 방향: `ASC`, `DESC`
- 구현 위치: Parser에서 문법 해석, Executor에서 `qsort` 수행
- 보강: NULL 값 정렬/출력 처리
- 제약: 다중 컬럼 ORDER BY 미지원

### 7. 아키텍처 비교 — 우리 프로젝트 vs MySQL vs SQLite

| 항목        | 우리 프로젝트            | MySQL                         | SQLite                                   |
| --------- | ------------------ | ----------------------------- | ---------------------------------------- |
| 목표        | SQL 처리 흐름 학습용 | 범용 서버형 RDBMS                  | 임베디드 파일 기반 DB                            |
| 실행 형태     | 단일 CLI 프로그램        | 클라이언트-서버 구조                   | 라이브러리 + CLI                              |
| SQL 처리 단계 | CLI -> Lexer -> Parser -> Executor    | Parser → Optimizer → Executor → Engine| Parser → Query Planner → Virtual Machine |
| 쿼리 최적화    | 없음                 | 있음 (인덱스, 실행계획)                | 있음 (경량 Planner)                          |
| 실행 방식     | 직접 파일 읽기/쓰기        | Storage Engine (InnoDB 등)     | VDBE(가상머신)로 실행                           |
| 저장 구조     | 단순 텍스트 파일 (.tbl)   | 페이지 기반 디스크 저장                 | 단일 파일 + B-tree 구조                        |
| 동시성 처리    | 없음                 | 트랜잭션 / 락 지원                   | 제한적 트랜잭션                                 |
| 지원 SQL    | INSERT, SELECT     | 대부분 SQL                       | 대부분 SQL                                  |

---

## 테스트케이스 및 엣지케이스

- 테스트 구성: 모듈별 단위 테스트 + 실행 흐름 검증
- 포함 범위: Lexer, Parser, Schema, Storage, Executor, CLI

**기본 테스트** — 핵심 동작 검증

| 구분 | 케이스 |
|------|--------|
| Lexer | 빈 입력, 키워드 대소문자 무시, 문자열 리터럴, 주석 처리, 비교 연산자 인식, 미종결 문자열 차단 |
| Parser | `SELECT *`, 다중 컬럼 SELECT, WHERE AND/OR, INSERT NULL 값, ORDER BY, FROM 누락 오류 |
| Schema | members 스키마 로드, 컬럼 조회, 타입 파싱, 존재하지 않는 스키마 파일, 다중 PK 스키마 차단, 빈 `.schema` 차단 |
| Storage | 첫 INSERT 시 헤더 생성, 다중 INSERT append, WHERE 기반 필터링, NULL round-trip, 빈 `.tbl` 차단, 빈 `.tbl` 복구 INSERT, 기존 PK 중복 차단 |
| Executor | PK 중복 차단, UTF-8 VARCHAR 길이 초과 차단, 잘못된 SELECT/WHERE/ORDER BY 컬럼 차단, NULL 허용 여부 검증 |
| CLI | `-e`, `-f` 실행, 다중 SQL 문장 실행, 파싱 오류 종료 코드, 실행 오류 시 스크립트 중단 |

**엣지 케이스** — 경계 상황 검증

| 구분 | 케이스 |
|------|--------|
| NULL 처리 | nullable 컬럼에서만 허용, NULL과 빈 문자열 저장 결과 구분, WHERE에서 NULL 비교 지원 |
| WHERE 제약 | 하나의 WHERE 절에서 AND/OR 혼합 불가, 괄호식 미지원, 존재하지 않는 컬럼 조건 처리 |
| ORDER BY 제약 | 단일 컬럼만 지원, 존재하지 않는 컬럼 정렬 차단, ASC/DESC 동작, NULL 정렬 처리 |
| 저장 제약 | 개행 문자가 포함된 값 저장 불가, `.tbl` 헤더와 `.schema` 불일치, 필드 수 불일치 행 차단, 기존 PK 중복 row 차단 |
| 타입 검증 | INT, FLOAT, DATE 형식 검사, 음수/부호/지수 숫자 처리, 오버플로/언더플로 입력 |
| 길이 제한 | `MAX_COLUMNS`, `MAX_CONDITIONS`, `MAX_TOKEN_LEN`, VARCHAR 최대 길이 경계값 검사, UTF-8 문자 수 기준 길이 검증 |
| 파일 처리 | 빈 `.tbl` 차단, 빈 `.schema` 차단, 긴 행 입력, 잘못된 숫자/날짜 row 차단, 없는 `.schema`/`.tbl`/SQL 파일 처리 |
| 실행 제약 | 스크립트 중간 실행 실패 시 이후 문장 중단, `MAX_ROWS` 초과 결과 차단 |
| 문법 오류 | 세미콜론 누락, 잘못된 키워드, ORDER BY 문법 오류, VALUES 개수 불일치 |
