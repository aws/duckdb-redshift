# Authentication and IAM permissions

The extension signs in with IAM only: it gets temporary database credentials for your AWS identity
from Redshift, and the database user is derived from that identity. Each `ATTACH` names exactly one
source of AWS credentials; there is no default, except for [ATTACH statements written for the aws
extension](attach-options.md#attach-statements-written-for-the-aws-extension).

| Source | `ATTACH` options |
|---|---|
| The AWS default credential chain: environment variables, the shared config and credentials files (with `AWS_PROFILE`), container and instance credentials | `CREDENTIAL_PROVIDER aws_default_chain` |
| A named profile in the AWS config or credentials file | `CREDENTIAL_PROVIDER profile, PROFILE 'my-profile'` |
| A DuckDB `aws` or `s3` secret | `SECRET my_secret` |

```sql
ATTACH '' AS rs (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', CREDENTIAL_PROVIDER profile, PROFILE 'my-profile');
```

## Profiles

A profile in the AWS config file (`~/.aws/config`, or the file `AWS_CONFIG_FILE` names) that uses
`credential_process` works, whether named by `PROFILE` or selected by `AWS_PROFILE`:

- With `PROFILE`, a profile of the same name in the shared credentials file takes precedence, and if
  the process fails, the connection fails.
- Selected by `AWS_PROFILE` under `aws_default_chain`, a failed process lets the default chain go on
  to its other sources, such as container or instance credentials, so the connection can succeed as
  that identity.

A profile that assumes a role (`role_arn` with `source_profile`) works only when `PROFILE` names it:
`CREDENTIAL_PROVIDER profile, PROFILE '<name>'`. Selected by `AWS_PROFILE` under
`CREDENTIAL_PROVIDER aws_default_chain`, the chain does not assume the role and never runs its
source, so the connection fails with "no usable AWS credentials", and the message says to name the
profile with `PROFILE`. On a host whose instance or container credentials the chain can reach, such
as an Amazon EC2 instance with an instance role, the chain goes on to those instead, and the
connection can succeed as the instance's identity rather than the role's: on such a host, name a
role profile with `PROFILE`, and check [the database user](#the-database-user) with
`SELECT current_user`.

## A DuckDB secret

`SECRET` takes the name of an `aws` or `s3` secret, such as one DuckDB's `aws` extension creates
from the credential chain. `aws` is a core DuckDB extension: DuckDB installs it from its own
repository and loads it when a statement needs it, as this `CREATE SECRET` does, so run
`INSTALL aws` and `LOAD aws` yourself only where `autoinstall_known_extensions` or
`autoload_known_extensions` is turned off.

```sql
LOAD redshift FROM aws.amazon.com;
CREATE SECRET my_secret (TYPE aws, PROVIDER credential_chain, REGION 'us-east-1');
ATTACH '' AS rs (TYPE redshift, WORKGROUP 'my-workgroup', ACCOUNT_ID '111122223333',
    REGION 'us-east-1', SECRET my_secret);
```

- `SECRET` is always the secret's name, never a key.
- `ATTACH` reads the secret, and refuses one that does not exist, is not of type `aws` or `s3`, or
  holds no key ID or secret key. The extension reads it again each time it opens a connection, so a
  replaced secret takes effect at the next connection; a session already open keeps the identity it
  connected with.
- The extension does not refresh a secret's credentials. When temporary credentials in a secret
  expire, the next connection fails; create the secret again.
- Without `REGION` or an endpoint path, the region is the secret's own `REGION`, which the `aws`
  extension may have taken from the environment or a profile and which may not be the target's. Give
  `REGION` with `WORKGROUP` or `CLUSTER`.

## IAM Identity Center

To use an IAM Identity Center (SSO) sign-in, sign in first with `aws sso login --profile
my-profile`, then create a DuckDB `aws` secret from that profile and name it in `SECRET`:

```sql
CREATE SECRET my_sso (TYPE aws, PROVIDER credential_chain, CHAIN 'sso', PROFILE 'my-profile');
```

The secret holds the IAM credentials of the sign-in, so the extension still signs in with IAM, not
through the driver's IAM Identity Center plugins ([What is not supported](#what-is-not-supported)).
If it holds none, the error says to run `aws sso login` and create the secret again.

## What is not supported

The Redshift driver's own sign-in plugins, for IAM Identity Center (browser and identity-token
modes) or an identity provider such as Okta, AD FS or Azure AD, a database user name and password,
and Kerberos are not supported. An `ATTACH` option or `CREDENTIAL_PROVIDER` value that asks for one
is refused; one that names a user, password, sign-in plugin, SSO or token says that only IAM
authentication is supported.

## IAM permissions

The AWS identity needs these actions for each kind of target:

| Target | Actions and resources |
|---|---|
| Serverless workgroup | `redshift-serverless:GetCredentials` on `arn:aws:redshift-serverless:<region>:<account>:workgroup/<workgroup-id>` |
| Labelled Serverless workgroup endpoint | `redshift-serverless:GetCredentials` on `arn:aws:redshift-serverless:<region>:<account>:workgroup/<label>/<workgroup>`; such an endpoint may need further IAM actions |
| Provisioned cluster, attached by its endpoint | `redshift:GetClusterCredentialsWithIAM` on `arn:aws:redshift:<region>:<account>:dbname:<cluster>/<database>` |
| Provisioned cluster, attached by `CLUSTER` | `redshift:DescribeClusters` on the cluster, and `redshift:GetClusterCredentialsWithIAM` as above |

For a cluster named by `CLUSTER`, the driver may first call
`redshift:DescribeCustomDomainAssociations`; when that is denied it goes on without it, so no grant
is needed.

A policy for a Serverless workgroup:

```json
{
  "Version": "2012-10-17",
  "Statement": [
    {
      "Effect": "Allow",
      "Action": "redshift-serverless:GetCredentials",
      "Resource": "arn:aws:redshift-serverless:us-east-1:111122223333:workgroup/<workgroup-id>"
    }
  ]
}
```

## The database user

Redshift derives the database user from the IAM identity: for a role, a user named after the role,
such as `IAMR:my-role`. What it may do in the database depends on its grants, as for any database
user, so a new user may need grants before it can read your tables. On a provisioned cluster, for
example, the user needs `USAGE` on the `awsdatacatalog` database to read the auto-mounted AWS Glue
Data Catalog, and `CREATE` on `dev` to create tables there. A query shows the user:

```sql
SELECT * FROM redshift_query('rs', 'SELECT current_user');
```
