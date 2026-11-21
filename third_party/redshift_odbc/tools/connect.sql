ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP 'analytics', ACCOUNT_ID '012345678901'); SELECT * FROM redshift_query('rs', 'SELECT 1');
