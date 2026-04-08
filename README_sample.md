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
- 제약: 괄호 우선순위 미지원
- 제약: 한 WHERE 절에서 `AND` / `OR` 혼합 불가

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
- 한계: 전체 파일 순회 필요
- 한계: 동시성 / 대용량 처리 부적합

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