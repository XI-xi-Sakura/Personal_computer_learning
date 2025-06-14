for port in $(seq 1 9); do
    # 创建 Redis 实例目录
    mkdir -p redis${port}/

    # 写入 Redis 配置文件
    cat << EOF > redis${port}/redis.conf
port 6379
bind 0.0.0.0
protected-mode no
appendonly yes
cluster-enabled yes                    # 开启集群模式
cluster-config-file nodes.conf         # 配置文件
cluster-node-timeout 5000              # 节点超时时间
cluster-announce-ip 172.30.0.10${port} # 节点 IP
cluster-announce-port 6379             # 节点端口
cluster-announce-bus-port 16379        # 节点总线端口
EOF
done


for port in $(seq 10 11); do
    # 创建 Redis 实例目录
    mkdir -p redis${port}/

    # 写入 Redis 配置文件
    cat << EOF > redis${port}/redis.conf
port 6379
bind 0.0.0.0
protected-mode no
appendonly yes
cluster-enabled yes
cluster-config-file nodes.conf
cluster-node-timeout 5000
cluster-announce-ip 172.30.0.1${port}
cluster-announce-port 6379
cluster-announce-bus-port 16379
EOF
done